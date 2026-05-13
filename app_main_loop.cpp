#include "worldsim_app_internal.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "layer_state_io.h"
#include "layer_import.h"
#include "feature_props.h"
#include "filters.h"
#include "filter_context_builder.h"
#include "geo.h"
#include "cache_io.h"
#include "arkavo_realtime_client.h"
#include "arkavo_signaling_transport_curl.h"
#include "arkavo_rtc_session_manager.h"
#include "time_cube.h"
#include "app_settings.h"
#include "dataset_library.h"
#include "net_http_utils.h"
#include "profiling.h"
#include "profiling_layer_snapshot.h"
#include "basemap_coverage.h"
#include "layer_runtime.h"
#include "layer_pipeline_drain.h"
#include "derived_layer_caches.h"
#include "layer_geometry.h"
#include "layer_workers.h"
#include "memory_utils.h"
#include "aggregate_visualization_strategies.h"
#include "map_render_heatmap_pass.h"
#include "map_render_basemap.h"
#include "map_render_hover.h"
#include "map_render_hud.h"
#include "map_inspection.h"
#include "map_overlay_panels.h"
#include "map_render_layers.h"
#include "render_plan_builder.h"
#include "render_layer_pass.h"
#include "render_tail_pass.h"
#include "map_render_overlays.h"
#include "map_render_projection.h"
#include "map_render_selection.h"
#include "map_render_utils.h"
#include "map_canvas_session.h"
#include "map_frame_render.h"
#include "map_frame_session.h"
#include "map_tab.h"
#include "map_viewport.h"
#include "parcel_timeline.h"
#include "parcel_unified.h"
#include "heatmap_render.h"
#include "heatmap_runtime.h"
#include "heat_normalization.h"
#include "heatmap_key_builder.h"
#include "render_policy.h"
#include "gear_panel.h"
#include "time_cube_panel.h"
#include "policy_panel.h"
#include "model_tabs_panel.h"
#include "layers_panel_ui.h"
#include "layer_settings.h"
#include "layer_runtime_coordinator.h"
#include "layer_ui_state_sync.h"
#include "layer_ui_context_builders.h"
#include "left_panel.h"
#include "frame_prelude.h"
#include "download_queue.h"
#include "basemap_panel.h"
#include "layer_download_queue.h"
#include "lan_discovery.h"
#include "data_library_coordinator.h"
#include "data_library_panel.h"
#include "performance_stats_panel.h"
#include "performance_runtime_support.h"
#include "tiles.h"
#include "owner_info.h"
#include "owners_tab.h"
#include "owner_aggregates.h"
#include "gradient_tab.h"
#include "vacancy_parcel_tab.h"
#include "parcel_info_tab.h"
#include "filters_tab.h"
#include "right_panel.h"
#include "duckdb_analytics.h"
#include "sql_tab.h"
#include "selection.h"
#include "app_lifecycle.h"
#include "worldsim_app.h"
#include "app_utils.h"
#include "app_frame_support.h"
#include "zoning.h"
#include "zoning_filters_panel.h"
#include "vacancy_overlay.h"
#include "status_api.h"
#include "dataset_lan_api.h"
#include "worldsim_cli.h"
#include "worldsim_dataset_bootstrap.h"
#include "worldsim_bootstrap.h"
#include "worldsim_app_run_state.h"
#include "parcel_matched_layers.h"
#include "app_shutdown_context_builder.h"
#include "layer_registry.h"
#include "status_api_context_builder.h"
#include "api_control_commands.h"
#include "cpu_affinity.h"
#include "thread_utils.h"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "earcut.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

static void clearTileDiskPresenceCache() {
    // Legacy hook: disk-presence memoization was removed during run-loop refactor.
}


int runWorldSim3App(int argc, char** argv) {

    fs::path root = resolveAppRoot(fs::current_path(), argc > 0 ? argv[0] : nullptr);
    WorldSimRunState runtime;
    runtime.argc = argc;
    runtime.argv = argv;
    runtime.root = root;
    const WorldsimCliOptions cli_options = parseWorldsimCliOptions(argc, argv);
    const int cli_result = runWorldsimCliImmediate(root, cli_options);
    if (cli_result >= 0) return cli_result;

    auto& app_settings = runtime.app_settings;
    app_settings.vulkan_validation_enabled = g_EnableValidationLayers;
    app_settings = loadAppSettings(root, app_settings);

    int reserve_cores = cli_options.reserve_cores_set ? cli_options.reserve_cores : app_settings.reserve_cpu_cores;
    if (!cli_options.reserve_cores_set) {
        if (const char* env = std::getenv("WORLD_SIM3_RESERVE_CORES")) {
            reserve_cores = std::max(0, std::atoi(env));
        }
    }
    if (reserve_cores > 0) {
        std::string affinity_msg;
        if (applyReservedCpuCores(reserve_cores, affinity_msg)) {
            std::cerr << "[worldsim3] " << affinity_msg << "\n";
        } else {
            std::cerr << "[worldsim3] CPU reservation disabled: " << affinity_msg << "\n";
        }
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    preloadLayersFromEnvironment(root);
    ensureParcelMatchedEventLayers(root, false, &std::cerr);

    g_EnableValidationLayers = app_settings.vulkan_validation_enabled;
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    int initial_window_w = 1600;
    int initial_window_h = 1000;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
        int work_x = 0;
        int work_y = 0;
        int work_w = 0;
        int work_h = 0;
        glfwGetMonitorWorkarea(monitor, &work_x, &work_y, &work_w, &work_h);
        if (work_w > 0 && work_h > 0) {
            initial_window_w = std::min(initial_window_w, std::max(640, (int)std::floor((float)work_w * 0.90f)));
            initial_window_h = std::min(initial_window_h, std::max(420, (int)std::floor((float)work_h * 0.85f)));
        }
    }
    GLFWwindow* window = glfwCreateWindow(initial_window_w, initial_window_h, "Baltimore Vulkan Map", nullptr, nullptr);

    uint32_t extensions_count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
    SetupVulkan(extensions, extensions_count);

    VkSurfaceKHR surface;
    VkResult err = glfwCreateWindowSurface(g_Instance, window, g_Allocator, &surface);
    if (err != VK_SUCCESS) return 1;

    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVulkanWindow(wd, surface, w, h);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiContext* main_imgui_context = ImGui::GetCurrentContext();
    ImGui::StyleColorsLight();

    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.RenderPass = wd->RenderPass;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    {
        VkCommandPool command_pool = wd->Frames[wd->FrameIndex].CommandPool;
        VkCommandBuffer command_buffer = wd->Frames[wd->FrameIndex].CommandBuffer;
        check_vk_result(vkResetCommandPool(g_Device, command_pool, 0));
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk_result(vkBeginCommandBuffer(command_buffer, &begin_info));
        ImGui_ImplVulkan_CreateFontsTexture();
        check_vk_result(vkEndCommandBuffer(command_buffer));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE));
        check_vk_result(vkDeviceWaitIdle(g_Device));
        ImGui_ImplVulkan_DestroyFontsTexture();
    }

    GLFWwindow* download_queue_window = glfwCreateWindow(500, 320, "Download Queue", nullptr, nullptr);
    if (!download_queue_window) return 1;
    int main_x = 0;
    int main_y = 0;
    glfwGetWindowPos(window, &main_x, &main_y);
    glfwSetWindowPos(download_queue_window, main_x + 1620, main_y + 96);
    VkSurfaceKHR download_queue_surface;
    err = glfwCreateWindowSurface(g_Instance, download_queue_window, g_Allocator, &download_queue_surface);
    if (err != VK_SUCCESS) return 1;
    ImGui_ImplVulkanH_Window download_queue_wd{};
    int dq_w = 0;
    int dq_h = 0;
    glfwGetFramebufferSize(download_queue_window, &dq_w, &dq_h);
    SetupVulkanWindow(&download_queue_wd, download_queue_surface, dq_w, dq_h);
    bool download_queue_swapchain_rebuild = false;

    ImGuiContext* download_queue_imgui_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(download_queue_imgui_context);
    ImGui::StyleColorsLight();
    ImGui_ImplGlfw_InitForVulkan(download_queue_window, false);
    glfwSetWindowUserPointer(download_queue_window, download_queue_imgui_context);
    glfwSetWindowFocusCallback(download_queue_window, [](GLFWwindow* cb_window, int focused) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(glfwGetWindowUserPointer(cb_window)));
        ImGui_ImplGlfw_WindowFocusCallback(cb_window, focused);
    });
    glfwSetCursorEnterCallback(download_queue_window, [](GLFWwindow* cb_window, int entered) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(glfwGetWindowUserPointer(cb_window)));
        ImGui_ImplGlfw_CursorEnterCallback(cb_window, entered);
    });
    glfwSetCursorPosCallback(download_queue_window, [](GLFWwindow* cb_window, double x, double y) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(glfwGetWindowUserPointer(cb_window)));
        ImGui_ImplGlfw_CursorPosCallback(cb_window, x, y);
    });
    glfwSetMouseButtonCallback(download_queue_window, [](GLFWwindow* cb_window, int button, int action, int mods) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(glfwGetWindowUserPointer(cb_window)));
        ImGui_ImplGlfw_MouseButtonCallback(cb_window, button, action, mods);
    });
    glfwSetScrollCallback(download_queue_window, [](GLFWwindow* cb_window, double xoffset, double yoffset) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(glfwGetWindowUserPointer(cb_window)));
        ImGui_ImplGlfw_ScrollCallback(cb_window, xoffset, yoffset);
    });
    glfwSetKeyCallback(download_queue_window, [](GLFWwindow* cb_window, int key, int scancode, int action, int mods) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(glfwGetWindowUserPointer(cb_window)));
        ImGui_ImplGlfw_KeyCallback(cb_window, key, scancode, action, mods);
    });
    glfwSetCharCallback(download_queue_window, [](GLFWwindow* cb_window, unsigned int c) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(glfwGetWindowUserPointer(cb_window)));
        ImGui_ImplGlfw_CharCallback(cb_window, c);
    });
    ImGui_ImplVulkan_InitInfo queue_init_info = init_info;
    queue_init_info.RenderPass = download_queue_wd.RenderPass;
    queue_init_info.ImageCount = download_queue_wd.ImageCount;
    ImGui_ImplVulkan_Init(&queue_init_info);
    {
        VkCommandPool command_pool = download_queue_wd.Frames[download_queue_wd.FrameIndex].CommandPool;
        VkCommandBuffer command_buffer = download_queue_wd.Frames[download_queue_wd.FrameIndex].CommandBuffer;
        check_vk_result(vkResetCommandPool(g_Device, command_pool, 0));
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk_result(vkBeginCommandBuffer(command_buffer, &begin_info));
        ImGui_ImplVulkan_CreateFontsTexture();
        check_vk_result(vkEndCommandBuffer(command_buffer));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE));
        check_vk_result(vkDeviceWaitIdle(g_Device));
        ImGui_ImplVulkan_DestroyFontsTexture();
    }
    ImGui::SetCurrentContext(main_imgui_context);

    BootstrapProgress bootstrap;
    bootstrap.running.store(false, std::memory_order_relaxed);
    bootstrap.done.store(true, std::memory_order_relaxed);
    bootstrap.phase.store(3, std::memory_order_relaxed);
    setBootstrapStatus(bootstrap, "Startup download disabled. Use Data Library to fetch missing datasets.");

    runtime.layers = loadManifest(root);
    auto& layers = runtime.layers;
    runtime.layer_registry.refresh(root, layers);
    auto& layer_registry = runtime.layer_registry;
    const WorldsimLayerIndices layer_indices = layer_registry.indices();
    const int parcel_layer_idx = layer_indices.parcel_layer_idx;
    const int real_property_layer_idx = layer_indices.real_property_layer_idx;
    const int vacant_notice_layer_idx = layer_indices.vacant_notice_layer_idx;
    const int vacant_rehab_layer_idx = layer_indices.vacant_rehab_layer_idx;
    const int tax_lien_layer_idx = layer_indices.tax_lien_layer_idx;
    const int tax_sale_layer_idx = layer_indices.tax_sale_layer_idx;
    const int zoning_layer_idx = layer_indices.zoning_layer_idx;
    const int crime_nibrs_layer_idx = layer_indices.crime_nibrs_layer_idx;
    const int crime_legacy_layer_idx = layer_indices.crime_legacy_layer_idx;
    std::unordered_map<std::string, size_t> real_property_by_blocklot;
    std::unordered_map<std::string, int> vacant_notice_count_by_blocklot;
    std::unordered_map<std::string, int> vacant_rehab_count_by_blocklot;
    std::unordered_map<std::string, int> tax_lien_count_by_blocklot;
    std::unordered_map<std::string, double> tax_lien_amount_by_blocklot;
    std::unordered_map<std::string, int> tax_sale_count_by_blocklot;
    std::unordered_map<std::string, double> tax_sale_amount_by_blocklot;
    std::unordered_map<std::string, bool> zoning_zone_enabled;
    std::unordered_map<std::string, ImVec4> zoning_zone_color;
    std::unordered_map<std::string, std::string> zoning_zone_label;
    std::unordered_map<std::string, ZoneMetadata> zoning_metadata = loadZoneMetadata(root);
    std::vector<std::string> zoning_zone_order;
    std::unordered_map<std::string, size_t> zoning_zone_counts;
    std::unordered_map<std::string, std::vector<std::string>> zoning_group_zones;
    std::vector<std::string> zoning_group_order;
    size_t zoning_zone_discovered_feature_count = 0;
    std::vector<int> parcel_vac_notice_by_feature;
    std::vector<int> parcel_vac_rehab_by_feature;
    std::vector<int> parcel_tax_lien_by_feature;
    std::vector<int> parcel_tax_sale_by_feature;
    std::vector<double> parcel_tax_lien_amount_by_feature;
    std::vector<double> parcel_tax_sale_amount_by_feature;
    int parcel_parameter_mode = 0;
    std::vector<UnifiedParcelRecord> unified_parcels;
    int vacancy_maps_generation = 0;
    int parcel_vacancy_generation_applied = -1;
    int tax_maps_generation = 0;
    int parcel_tax_generation_applied = -1;
    size_t unified_parcel_cached_size = (size_t)-1;
    size_t unified_real_property_cached_size = (size_t)-1;
    int unified_vacancy_generation_applied = -1;
    int unified_tax_generation_applied = -1;
    size_t cached_real_property_size = 0;
    size_t cached_vac_notice_size = 0;
    size_t cached_vac_rehab_size = 0;
    size_t cached_tax_lien_size = 0;
    size_t cached_tax_sale_size = 0;
    std::mutex hydrated_mutex;
    std::deque<HydratedLayer> hydrated_queue;
    std::mutex hydrate_req_mutex;
    std::condition_variable hydrate_req_cv;
    std::deque<size_t> hydrate_requests;
    std::mutex tri_mutex;
    std::condition_variable tri_cv;
    std::deque<TriJob> tri_jobs;
    std::deque<TriResult> tri_results;
    std::atomic<bool> hydration_stop{false};
    std::atomic<size_t> hydrated_count{0};
    std::atomic<size_t> triangulated_count{0};
    std::atomic<double> perf_frame_ms_avg{0.0};
    std::atomic<double> perf_frame_ms_last{0.0};
    std::atomic<double> perf_fps_avg{0.0};
    std::atomic<double> ui_left_panel_frac{0.34};
    std::atomic<double> ui_right_panel_frac{0.24};
    std::atomic<double> prof_ui_ms_last{0.0};
    std::atomic<double> prof_owner_ms_last{0.0};
    std::atomic<double> prof_tile_ms_last{0.0};
    std::atomic<double> prof_layer_ms_last{0.0};
    std::atomic<double> prof_heatmap_ms_last{0.0};
    std::atomic<double> prof_overlay_ms_last{0.0};
    std::atomic<double> prof_present_ms_last{0.0};
    std::atomic<size_t> prof_tiles_drawn_last{0};
    std::atomic<size_t> prof_features_considered_last{0};
    std::atomic<size_t> prof_features_drawn_last{0};
    std::atomic<size_t> prof_heat_samples_last{0};
    std::atomic<size_t> prof_retired_textures{0};
    std::atomic<bool> prof_heatmap_gpu_splat_active{false};
    std::atomic<bool> prof_heatmap_high_quality{false};
    std::atomic<bool> prof_heatmap_cache_valid{false};
    std::atomic<bool> prof_heatmap_texture_resident{false};
    std::atomic<bool> prof_heatmap_async_inflight{false};
    std::atomic<uint64_t> prof_heatmap_cache_key{0};
    std::atomic<size_t> prof_heatmap_texture_cache_entries{0};
    std::atomic<size_t> prof_tile_cache_size{0};
    std::mutex profile_mutex;
    std::vector<ProfileFrameSample> profile_samples(600);
    size_t profile_sample_pos = 0;
    size_t profile_sample_count = 0;
    uint64_t profile_reset_generation = 0;
    std::mutex layer_profile_mutex;
    std::vector<LayerProfileSnapshot> layer_profile_snapshot(layers.size());
    std::vector<bool> layer_profile_dirty(layers.size(), true);
    std::atomic<size_t> render_fill_attempts_last_frame{0};
    std::atomic<size_t> render_fill_success_last_frame{0};
    std::atomic<size_t> render_fill_no_triangles_last_frame{0};
    std::atomic<size_t> render_fill_bad_indices_last_frame{0};
    std::atomic<size_t> visible_vacant_parcels_last_frame{0};
    std::atomic<size_t> vacant_parcels_matched_total{0};
    std::atomic<size_t> vacant_parcels_with_geometry_total{0};
    std::atomic<size_t> vacant_parcels_triangulated_renderable_total{0};
    std::atomic<size_t> vacant_notice_rows_matched_total{0};
    std::atomic<size_t> vacant_rehab_rows_matched_total{0};
    std::atomic<int> current_zoom_state{12};
    std::atomic<double> current_lon_state{-76.6122};
    std::atomic<double> current_lat_state{39.2904};
    std::atomic<int> api_zoom_cmd{-1};
    std::atomic<double> api_lon_cmd{std::numeric_limits<double>::quiet_NaN()};
    std::atomic<double> api_lat_cmd{std::numeric_limits<double>::quiet_NaN()};
    std::atomic<uint64_t> api_ui_cmd_seq{0};
    std::atomic<int> api_ui_cmd_kind{0};
    std::atomic<double> api_ui_cmd_x{0.0};
    std::atomic<double> api_ui_cmd_y{0.0};
    std::atomic<int> api_ui_cmd_button{0};
    std::atomic<double> api_ui_cmd_scroll_y{0.0};
    std::mutex api_layer_mutex;
    std::unordered_map<std::string, bool> api_layer_enable_cmds;
    std::unordered_map<std::string, bool> api_layer_fill_cmds;
    std::vector<std::string> api_layer_download_cmds;
    std::mutex layer_fill_mutex;
    std::vector<bool> layer_fill_enabled(layers.size(), true);
    std::vector<bool> layer_hover_enabled(layers.size(), true);
    std::vector<bool> layer_inspect_enabled(layers.size(), true);
    std::vector<bool> layer_heatmap_enabled(layers.size(), true);
    std::vector<int> layer_heatmap_max_zoom(layers.size(), 13);
    std::vector<int> layer_parcel_detail_min_zoom(layers.size(), kParcelChoroplethMinZoom);
    std::vector<bool> layer_heatmap_use_gradient(layers.size(), true);
    std::vector<float> layer_choropleth_gamma(layers.size(), 1.0f);
    std::vector<int> layer_heatmap_algo(layers.size(), -1);
    std::vector<int> layer_normalize_mode(layers.size(), 0);
    std::vector<float> layer_heatmap_cell_px(layers.size(), 24.0f);
    std::vector<float> layer_heatmap_bandwidth_px(layers.size(), 18.0f);
    std::vector<float> layer_heatmap_blur_sigma_px(layers.size(), 6.0f);
    std::vector<float> layer_heatmap_percentile_clip(layers.size(), 95.0f);
    std::vector<bool> layer_heatmap_zoom_adaptive_bandwidth(layers.size(), true);
    std::vector<bool> layer_heatmap_multires_enabled(layers.size(), true);
    std::vector<float> layer_heatmap_multires_blend(layers.size(), 0.5f);
    auto& heatmap_runtime = runtime.heatmap;
    auto& global_heat_cell_px = heatmap_runtime.global_heat_cell_px;
    auto& heatmap_algo = heatmap_runtime.heatmap_algo;
    auto& heatmap_quality_preset = heatmap_runtime.heatmap_quality_preset;
    auto& heatmap_bandwidth_px = heatmap_runtime.heatmap_bandwidth_px;
    auto& heatmap_blur_sigma_px = heatmap_runtime.heatmap_blur_sigma_px;
    auto& heatmap_percentile_clip = heatmap_runtime.heatmap_percentile_clip;
    auto& heatmap_zoom_adaptive_bandwidth = heatmap_runtime.heatmap_zoom_adaptive_bandwidth;
    auto& heatmap_multires_enabled = heatmap_runtime.heatmap_multires_enabled;
    auto& heatmap_multires_blend = heatmap_runtime.heatmap_multires_blend;
    auto& heatmap_allow_cpu_fallback = heatmap_runtime.heatmap_allow_cpu_fallback;
    int hover_inspector_mode = 3; // 0=None, 1=Parcels, 2=Zoning, 3=Both
    bool hover_inspector_enabled = true;
    loadLayerUiState(
        root,
        layers,
        hover_inspector_enabled,
        &hover_inspector_mode,
        &zoning_zone_enabled,
        &layer_fill_enabled,
        &layer_hover_enabled,
        &layer_inspect_enabled,
        &layer_heatmap_enabled,
        &layer_heatmap_max_zoom,
        &layer_parcel_detail_min_zoom,
        &layer_heatmap_use_gradient,
        &layer_choropleth_gamma,
        &layer_heatmap_algo,
        &layer_normalize_mode,
        &layer_heatmap_cell_px,
        &layer_heatmap_bandwidth_px,
        &layer_heatmap_blur_sigma_px,
        &layer_heatmap_percentile_clip,
        &layer_heatmap_zoom_adaptive_bandwidth,
        &layer_heatmap_multires_enabled,
        &layer_heatmap_multires_blend,
        &heatmap_algo,
        &heatmap_quality_preset,
        &global_heat_cell_px,
        &heatmap_bandwidth_px,
        &heatmap_blur_sigma_px,
        &heatmap_percentile_clip,
        &heatmap_zoom_adaptive_bandwidth,
        &heatmap_multires_enabled,
        &heatmap_multires_blend,
        &heatmap_allow_cpu_fallback);
    if (heatmap_algo == kAggregateGridBinning) heatmap_algo = kAggregateHexBinning;
    if (heatmap_algo == kAggregateGpuSplatBlur) heatmap_quality_preset = 2;
    heatmap_quality_preset = std::clamp(heatmap_quality_preset, 0, 2);
    for (size_t i = 0; i < layers.size() && i < layer_heatmap_algo.size(); ++i) {
        std::string field = layers[i].heatmap_field;
        std::transform(field.begin(), field.end(), field.begin(), [](unsigned char c) {
            return (char)std::tolower(c);
        });
        if (layers[i].scale == "parcel" &&
            (field == "value_usd" || field == "property_value_usd") &&
            (layer_heatmap_algo[i] < 0 || layer_heatmap_algo[i] == kAggregateGridBinning)) {
            layer_heatmap_algo[i] = kAggregateMedianChoropleth;
            if (i < layer_normalize_mode.size()) layer_normalize_mode[i] = 0;
        } else if (layer_heatmap_algo[i] == kAggregateGridBinning) {
            layer_heatmap_algo[i] = kAggregateHexBinning;
        }
    }
    if (parcel_layer_idx >= 0) {
        for (size_t i = 0; i < layers.size(); ++i) {
            if ((int)i != parcel_layer_idx && layers[i].scale == "parcel") layers[i].enabled = false;
        }
    }
    TimeCubeService time_cube_service(root);
    DuckDbAnalytics duckdb_analytics(root);
    bool duckdb_auto_rebuild_checked = false;
    std::mutex status_mutex;
    std::vector<LayerRuntimeState> layer_states(layers.size());
    std::vector<LayerSpatialIndex> layer_spatial(layers.size());
    std::vector<OwnerAggregate> owner_aggregates;
    MapFilterState map_filter_state;
    auto& selected_owners = map_filter_state.selected_owners;
    std::vector<QueryMapLayer> query_layers;
    const std::array<const char*, 24> parcel_jurisdiction_options = {
        "Allegany County",
        "Anne Arundel County",
        "Baltimore City",
        "Baltimore County",
        "Calvert County",
        "Caroline County",
        "Carroll County",
        "Cecil County",
        "Charles County",
        "Dorchester County",
        "Frederick County",
        "Garrett County",
        "Harford County",
        "Howard County",
        "Kent County",
        "Montgomery County",
        "Prince George's County",
        "Queen Anne's County",
        "St. Mary's County",
        "Somerset County",
        "Talbot County",
        "Washington County",
        "Wicomico County",
        "Worcester County"
    };
    std::unordered_set<std::string> parcel_jurisdiction_filter;
    for (const char* jurisdiction : parcel_jurisdiction_options) parcel_jurisdiction_filter.insert(jurisdiction);
    bool parcel_jurisdiction_filter_dirty = true;
    FilterResultSet parcel_jurisdiction_result_set;
    std::string parcel_jurisdiction_filter_status = "All Maryland parcels";
    std::unordered_map<std::string, std::string> owner_class_overrides;
    bool owner_class_overrides_loaded = false;
    bool owner_class_overrides_dirty = false;
    int owner_class_filter_mode = 0; // 0=all
    int owner_class_assign_mode = 1; // default government
    int selected_owner_anchor = -1;
    FilteredAggregateSnapshot filtered_aggregate_snapshot;
    bool owner_aggregates_dirty = true;
    int owner_sort_mode = 0; // 0=count,1=area,2=value
    int owner_sorted_mode = -1;
    size_t owner_cached_parcel_size = (size_t)-1;
    size_t owner_cached_real_property_size = (size_t)-1;
    auto hydration_started_at = std::chrono::steady_clock::now();
    auto last_hydration_progress_at = hydration_started_at;
    auto last_tri_progress_at = hydration_started_at;
    size_t last_hydrated_seen = 0;
    size_t last_triangulated_seen = 0;

    std::vector<bool> hydration_requested(layers.size(), false);
    std::vector<bool> hydration_required(layers.size(), false);
    std::vector<size_t> initial_hydration_order;
    initial_hydration_order.reserve(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].enabled) initial_hydration_order.push_back(i);
    }
    auto layer_file_size = [&](size_t idx) -> uintmax_t {
        std::error_code ec;
        const fs::path p = root / "data" / "layers" / layers[idx].file;
        const uintmax_t sz = fs::file_size(p, ec);
        return ec ? std::numeric_limits<uintmax_t>::max() : sz;
    };
    auto startup_hydration_rank = [&](size_t idx) {
        if (parcel_layer_idx >= 0 && (int)idx == parcel_layer_idx) return 0;
        if (zoning_layer_idx >= 0 && (int)idx == zoning_layer_idx) return 1;
        const uintmax_t sz = layer_file_size(idx);
        if (layers[idx].file == "regional_parcels.geojson" || sz > 250ull * 1024ull * 1024ull) return 4;
        if (layers[idx].scale == "parcel") return 3;
        return 2;
    };
    std::stable_sort(initial_hydration_order.begin(), initial_hydration_order.end(), [&](size_t a, size_t b) {
        const int ar = startup_hydration_rank(a);
        const int br = startup_hydration_rank(b);
        if (ar != br) return ar < br;
        return layer_file_size(a) < layer_file_size(b);
    });
    for (size_t i : initial_hydration_order) {
        hydrate_requests.push_back(i);
        hydration_requested[i] = true;
    }
    auto is_parcel_priority_index = [&](size_t idx) -> bool {
        if (parcel_layer_idx < 0) return false;
        if ((int)idx == parcel_layer_idx) return true;
        const bool vac_active =
            (vacant_notice_layer_idx >= 0 && layers[(size_t)vacant_notice_layer_idx].enabled) ||
            (vacant_rehab_layer_idx >= 0 && layers[(size_t)vacant_rehab_layer_idx].enabled);
        return vac_active && (int)idx == parcel_layer_idx;
    };
    auto layer_data_available = [&](size_t idx) -> bool {
        if (idx >= layers.size() || layers[idx].features.empty()) return false;
        std::lock_guard<std::mutex> lk(status_mutex);
        if (idx >= layer_states.size()) return false;
        LayerPipelineStatus st = layer_states[idx].status;
        return st == LayerPipelineStatus::Hydrated ||
               st == LayerPipelineStatus::TriQueued ||
               st == LayerPipelineStatus::Triangulating ||
               st == LayerPipelineStatus::Ready;
    };
    auto enqueue_hydration = [&](size_t idx, bool required = false) {
        if (idx >= layers.size()) return;
        std::lock_guard<std::mutex> lk(hydrate_req_mutex);
        if (required) hydration_required[idx] = true;
        bool retry_failed = false;
        if (hydration_requested[idx]) {
            std::lock_guard<std::mutex> lk2(status_mutex);
            retry_failed = idx < layer_states.size() && layer_states[idx].status == LayerPipelineStatus::Failed;
        }
        if (!hydration_requested[idx] || retry_failed) {
            if (is_parcel_priority_index(idx)) hydrate_requests.push_front(idx);
            else hydrate_requests.push_back(idx);
            hydration_requested[idx] = true;
            std::lock_guard<std::mutex> lk2(status_mutex);
            if (idx < layer_states.size()) layer_states[idx].status = LayerPipelineStatus::Queued;
        }
        hydrate_req_cv.notify_one();
    };

    const unsigned int hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned int hydration_worker_count = std::min(4u, hw);
    LayerWorkersContext layer_workers_ctx{
        root,
        &layers,
        &hydration_stop,
        &hydrate_req_mutex,
        &hydrate_req_cv,
        &hydrate_requests,
        &hydration_requested,
        &hydration_required,
        &hydrated_mutex,
        &hydrated_queue,
        &status_mutex,
        &layer_states,
        &tri_mutex,
        &tri_cv,
        &tri_jobs,
        &tri_results
    };
    std::vector<std::thread> hydration_workers = startHydrationWorkers(layer_workers_ctx, hydration_worker_count);
    std::thread triangulation_worker = startTriangulationWorker(layer_workers_ctx);

    StatusApiContextFactoryInput status_api_input;
    status_api_input.app_version = kAppVersion;
    status_api_input.protocol_version = kProtocolVersion;
    status_api_input.tile_cache_max = kMaxTileCache;
    status_api_input.stop = &hydration_stop;
    status_api_input.layers = &layers;
    status_api_input.time_cube_service = &time_cube_service;
    status_api_input.screenshot = &g_ScreenshotState;
    status_api_input.status_mutex = &status_mutex;
    status_api_input.layer_states = &layer_states;
    status_api_input.layer_fill_mutex = &layer_fill_mutex;
    status_api_input.layer_fill_enabled = &layer_fill_enabled;
    status_api_input.layer_hover_enabled = &layer_hover_enabled;
    status_api_input.layer_inspect_enabled = &layer_inspect_enabled;
    status_api_input.layer_heatmap_enabled = &layer_heatmap_enabled;
    status_api_input.hydration_started_at = &hydration_started_at;
    status_api_input.hydrated_count = &hydrated_count;
    status_api_input.triangulated_count = &triangulated_count;
    status_api_input.prof_tile_cache_size = &prof_tile_cache_size;
    status_api_input.current_zoom_state = &current_zoom_state;
    status_api_input.current_lon_state = &current_lon_state;
    status_api_input.current_lat_state = &current_lat_state;
    status_api_input.visible_vacant_parcels_last_frame = &visible_vacant_parcels_last_frame;
    status_api_input.vacant_parcels_matched_total = &vacant_parcels_matched_total;
    status_api_input.vacant_parcels_with_geometry_total = &vacant_parcels_with_geometry_total;
    status_api_input.vacant_parcels_triangulated_renderable_total = &vacant_parcels_triangulated_renderable_total;
    status_api_input.perf_frame_ms_avg = &perf_frame_ms_avg;
    status_api_input.perf_frame_ms_last = &perf_frame_ms_last;
    status_api_input.perf_fps_avg = &perf_fps_avg;
    status_api_input.ui_left_panel_frac = &ui_left_panel_frac;
    status_api_input.ui_right_panel_frac = &ui_right_panel_frac;
    status_api_input.render_fill_attempts_last_frame = &render_fill_attempts_last_frame;
    status_api_input.render_fill_success_last_frame = &render_fill_success_last_frame;
    status_api_input.render_fill_no_triangles_last_frame = &render_fill_no_triangles_last_frame;
    status_api_input.render_fill_bad_indices_last_frame = &render_fill_bad_indices_last_frame;
    status_api_input.api_layer_mutex = &api_layer_mutex;
    status_api_input.api_layer_enable_cmds = &api_layer_enable_cmds;
    status_api_input.api_layer_fill_cmds = &api_layer_fill_cmds;
    status_api_input.api_layer_download_cmds = &api_layer_download_cmds;
    status_api_input.api_zoom_cmd = &api_zoom_cmd;
    status_api_input.api_lon_cmd = &api_lon_cmd;
    status_api_input.api_lat_cmd = &api_lat_cmd;
    status_api_input.api_ui_cmd_seq = &api_ui_cmd_seq;
    status_api_input.api_ui_cmd_kind = &api_ui_cmd_kind;
    status_api_input.api_ui_cmd_x = &api_ui_cmd_x;
    status_api_input.api_ui_cmd_y = &api_ui_cmd_y;
    status_api_input.api_ui_cmd_button = &api_ui_cmd_button;
    status_api_input.api_ui_cmd_scroll_y = &api_ui_cmd_scroll_y;
    status_api_input.layer_profile_mutex = &layer_profile_mutex;
    status_api_input.layer_profile_snapshot = &layer_profile_snapshot;
    status_api_input.profile_mutex = &profile_mutex;
    status_api_input.profile_samples = &profile_samples;
    status_api_input.profile_sample_pos = &profile_sample_pos;
    status_api_input.profile_sample_count = &profile_sample_count;
    status_api_input.profile_reset_generation = &profile_reset_generation;
    status_api_input.prof_ui_ms_last = &prof_ui_ms_last;
    status_api_input.prof_owner_ms_last = &prof_owner_ms_last;
    status_api_input.prof_tile_ms_last = &prof_tile_ms_last;
    status_api_input.prof_layer_ms_last = &prof_layer_ms_last;
    status_api_input.prof_heatmap_ms_last = &prof_heatmap_ms_last;
    status_api_input.prof_overlay_ms_last = &prof_overlay_ms_last;
    status_api_input.prof_present_ms_last = &prof_present_ms_last;
    status_api_input.prof_tiles_drawn_last = &prof_tiles_drawn_last;
    status_api_input.prof_features_considered_last = &prof_features_considered_last;
    status_api_input.prof_features_drawn_last = &prof_features_drawn_last;
    status_api_input.prof_heat_samples_last = &prof_heat_samples_last;
    status_api_input.prof_retired_textures = &prof_retired_textures;
    status_api_input.prof_heatmap_gpu_splat_active = &prof_heatmap_gpu_splat_active;
    status_api_input.prof_heatmap_high_quality = &prof_heatmap_high_quality;
    status_api_input.prof_heatmap_cache_valid = &prof_heatmap_cache_valid;
    status_api_input.prof_heatmap_texture_resident = &prof_heatmap_texture_resident;
    status_api_input.prof_heatmap_async_inflight = &prof_heatmap_async_inflight;
    status_api_input.prof_heatmap_cache_key = &prof_heatmap_cache_key;
    status_api_input.prof_heatmap_texture_cache_entries = &prof_heatmap_texture_cache_entries;
    std::thread status_api_worker = startStatusApiWorker(makeStatusApiContext(status_api_input));

    std::mutex p2p_mutex;
    std::unordered_map<std::string, std::vector<json>> p2p_mailbox;
    std::thread dataset_api_worker = startDatasetApiWorker(DatasetLanApiContext{
        kAppVersion,
        kProtocolVersion,
        root,
        &hydration_stop,
        &p2p_mutex,
        &p2p_mailbox
    });
    std::thread lan_discovery_worker = startLanDiscoveryWorker(DatasetLanApiContext{
        kAppVersion,
        kProtocolVersion,
        root,
        &hydration_stop,
        &p2p_mutex,
        &p2p_mailbox
    });



    int zoom = 12;
    double center_lon = -76.6122;
    double center_lat = 39.2904;
    float ui_text_scale = 1.0f;
    uint64_t api_ui_cmd_last_seq = 0;
    bool api_ui_mouse_release_pending = false;
    int api_ui_mouse_release_button = 0;
    std::vector<bool> last_enabled_state;
    last_enabled_state.reserve(layers.size());
    for (const auto& l : layers) last_enabled_state.push_back(l.enabled);
    int last_hover_inspector_mode = hover_inspector_mode;
    bool show_sources_panel = false;
    bool show_data_library = false;
    char data_library_query[128] = "";
    auto& data_library_state = runtime.data_library;
    auto& data_library_status_msg = data_library_state.status_msg;
    auto& data_library_cached_query = data_library_state.cached_query;
    auto& data_library_cached_layer_count = data_library_state.cached_layer_count;
    auto& data_library_visible_rows = data_library_state.visible_rows;
    auto& data_library_cache_rebuilds = data_library_state.cache_rebuilds;
    auto& data_library_rendered_rows_last = data_library_state.rendered_rows_last;
    auto& data_freshness_state = data_library_state.freshness_state;
    auto& data_freshness_msg = data_library_state.freshness_msg;
    auto& local_layer_exists_cache = data_library_state.local_layer_exists_cache;
    data_freshness_state.assign(layers.size(), FreshnessState::Unknown);
    data_freshness_msg.assign(layers.size(), "");
    local_layer_exists_cache.assign(layers.size(), false);
    auto refresh_local_layer_exists_cache = [&]() {
        for (size_t i = 0; i < layers.size(); ++i) {
            std::error_code ec;
            local_layer_exists_cache[i] = fs::exists(root / "data" / "layers" / layers[i].file, ec) && !ec;
        }
    };
    auto mark_local_layer_exists = [&](size_t idx, bool exists) {
        if (idx < local_layer_exists_cache.size()) local_layer_exists_cache[idx] = exists;
    };
    refresh_local_layer_exists_cache();
    auto& data_library_download_phase = data_library_state.download_phase;
    auto& data_library_include_large = data_library_state.include_large;
    auto& data_library_bulk_inflight = data_library_state.bulk_inflight;
    auto& data_library_bulk_mutex = data_library_state.bulk_mutex;
    auto& data_library_bulk_progress = data_library_state.bulk_progress;
    auto& data_library_bulk_future = data_library_state.bulk_future;
    DownloadQueueState basemap_download;
    LazyTileDownloadState lazy_tile_download;
    std::deque<size_t> layer_download_queue;
    bool layer_download_inflight = false;
    size_t layer_download_active_idx = (size_t)-1;
    std::future<VersionedDownloadResult> layer_download_future;
    std::string layer_download_active_file;
    std::string layer_download_last_event;
    bool layer_download_queue_loaded = false;
    auto& filter_enabled = map_filter_state.enabled;
    auto& filter_use_date = map_filter_state.use_date;
    auto& filter_year_min = map_filter_state.year_min;
    auto& filter_year_max = map_filter_state.year_max;
    auto& filter_blocklot = map_filter_state.blocklot;
    auto& filter_status = map_filter_state.status;
    auto& filter_address = map_filter_state.address;
    std::string address_locate_status;
    std::vector<AddressLocateMatch> address_locate_matches;
    auto& filter_owner = map_filter_state.owner;
    char owner_search_query[96] = "";
    char owner_info_property_query[128] = "";
    ElementInfoUiState element_info_state{{}, (size_t)-1, false, owner_info_property_query, sizeof(owner_info_property_query)};
    ParcelSelectionState parcel_selection;
    bool& show_selected_parcel_details = parcel_selection.show_details;
    size_t& selected_parcel_idx = parcel_selection.active_idx;
    auto& selected_parcel_indices = parcel_selection.indices;
    auto& selected_parcel_index_set = parcel_selection.index_set;
    auto& filter_zip = map_filter_state.zip;
    auto& crime_filter_enabled = map_filter_state.crime.enabled;
    auto& crime_filter_homicide = map_filter_state.crime.homicide;
    auto& crime_filter_robbery = map_filter_state.crime.robbery;
    auto& crime_filter_assault = map_filter_state.crime.assault;
    auto& crime_filter_burglary = map_filter_state.crime.burglary;
    auto& crime_filter_theft = map_filter_state.crime.theft;
    auto& crime_filter_auto_theft = map_filter_state.crime.auto_theft;
    auto& crime_filter_drug = map_filter_state.crime.drug;
    auto& crime_filter_shooting = map_filter_state.crime.shooting;
    auto& crime_filter_use_year = map_filter_state.crime.use_year;
    auto& crime_year_min = map_filter_state.crime.year_min;
    auto& crime_year_max = map_filter_state.crime.year_max;
    loadFilterUiState(
        root,
        &filter_enabled,
        &filter_use_date,
        &filter_year_min,
        &filter_year_max,
        filter_blocklot,
        sizeof(filter_blocklot),
        filter_status,
        sizeof(filter_status),
        filter_address,
        sizeof(filter_address),
        filter_owner,
        sizeof(filter_owner),
        filter_zip,
        sizeof(filter_zip),
        &crime_filter_enabled,
        &crime_filter_homicide,
        &crime_filter_robbery,
        &crime_filter_assault,
        &crime_filter_burglary,
        &crime_filter_theft,
        &crime_filter_auto_theft,
        &crime_filter_drug,
        &crime_filter_shooting,
        &crime_filter_use_year,
        &crime_year_min,
        &crime_year_max,
        owner_search_query,
        sizeof(owner_search_query),
        &selected_owners);
    loadMapUiState(
        root,
        &center_lon,
        &center_lat,
        &zoom,
        &selected_parcel_idx,
        &selected_parcel_indices);
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    center_lat = std::clamp(center_lat, -85.0, 85.0);
    selected_parcel_index_set.clear();
    for (size_t idx : selected_parcel_indices) selected_parcel_index_set.insert(idx);
    show_selected_parcel_details = !selected_parcel_indices.empty() && selected_parcel_idx != (size_t)-1;
    std::vector<std::pair<std::string, int>> crime_breakdown;
    std::vector<int> record_year_hist(201, 0); // 1900..2100
    std::vector<float> record_year_hist_plot(201, 0.0f);
    std::vector<size_t> hist_feature_counts(layers.size(), 0);
    std::vector<bool> hist_enabled(layers.size(), false);
    bool hist_dirty = true;
    float record_year_hist_max_bin = 1.0f;
    int record_year_nonzero_min = 1900;
    int record_year_nonzero_max = 2100;
    int record_year_nonzero_total = 0;
    int selected_record_year = -1;
    bool selected_record_year_dirty = true;
    int selected_record_year_total = 0;
    std::vector<std::string> selected_record_year_samples;
    bool show_selected_zone_details = false;
    size_t selected_zone_idx = (size_t)-1;
    bool basemap_source_has_any_files_cached = false;
    bool topo_tiles_available_cached = false;
    bool topo_vector_available_cached = false;
    size_t osm_missing_tiles_cached = 0;
    size_t osm_total_tiles_cached = 0;
    size_t topo_missing_tiles_cached = 0;
    size_t topo_total_tiles_cached = 0;
    bool basemap_coverage_dirty = true;
    constexpr auto kBasemapCoverageRefreshInterval = std::chrono::seconds(2);
    std::string tile_root_dir_cached = "tiles";
    auto basemap_availability_last_check = std::chrono::steady_clock::time_point{};
    auto last_frame_ts = std::chrono::steady_clock::now();
    double ema_frame_ms = 0.0;
    constexpr double kPerfAlpha = 0.12;
    std::string last_cache_clear_msg;
    bool clear_cache_all = true;
    bool clear_cache_hydration = true;
    bool clear_cache_triangulation = true;
    bool clear_cache_derived = true;
    bool clear_cache_heatmap_memory = true;
    bool clear_cache_heatmap_disk = true;
    bool clear_cache_tile_memory = true;
    bool clear_cache_tile_disk_presence = true;
    const fs::path cache_root_dir = root / "data" / "cache";
    const fs::path cache_hydration_dir = cache_root_dir / "hydration";
    const fs::path cache_triangulation_dir = cache_root_dir / "triangulation";
    const fs::path cache_derived_dir = cache_root_dir / "derived";
    const fs::path cache_aggregate_dir = cache_root_dir / "aggregate";
    auto clear_cache_tree = [&](const fs::path& p) {
        std::error_code ec;
        if (!fs::exists(p, ec)) return (size_t)0;
        return fs::remove_all(p, ec);
    };
    auto reset_derived_cache_state = [&]() {
        cached_real_property_size = 0;
        cached_vac_notice_size = 0;
        cached_vac_rehab_size = 0;
        cached_tax_lien_size = 0;
        cached_tax_sale_size = 0;
        vacancy_maps_generation = 0;
        parcel_vacancy_generation_applied = -1;
        tax_maps_generation = 0;
        parcel_tax_generation_applied = -1;
        owner_aggregates_dirty = true;
        releaseContainerStorage(parcel_vac_notice_by_feature);
        releaseContainerStorage(parcel_vac_rehab_by_feature);
        releaseContainerStorage(parcel_tax_lien_by_feature);
        releaseContainerStorage(parcel_tax_sale_by_feature);
        releaseContainerStorage(parcel_tax_lien_amount_by_feature);
        releaseContainerStorage(parcel_tax_sale_amount_by_feature);
        releaseContainerStorage(vacant_notice_count_by_blocklot);
        releaseContainerStorage(vacant_rehab_count_by_blocklot);
        releaseContainerStorage(tax_lien_count_by_blocklot);
        releaseContainerStorage(tax_lien_amount_by_blocklot);
        releaseContainerStorage(tax_sale_count_by_blocklot);
        releaseContainerStorage(tax_sale_amount_by_blocklot);
        releaseContainerStorage(real_property_by_blocklot);
        releaseContainerStorage(zoning_zone_label);
        releaseContainerStorage(zoning_zone_counts);
        releaseContainerStorage(zoning_zone_order);
        releaseContainerStorage(zoning_group_zones);
        releaseContainerStorage(zoning_group_order);
        zoning_zone_discovered_feature_count = 0;
        visible_vacant_parcels_last_frame.store(0, std::memory_order_relaxed);
        vacant_parcels_matched_total.store(0, std::memory_order_relaxed);
        vacant_parcels_with_geometry_total.store(0, std::memory_order_relaxed);
        vacant_parcels_triangulated_renderable_total.store(0, std::memory_order_relaxed);
    };
    auto clear_heatmap_runtime_cache = [&]() {
        clearHeatmapRuntimeCache(heatmap_runtime);
    };
    TimeCubeResult time_cube_ui_result;
    bool time_cube_ui_loaded = false;
    std::string time_cube_ui_status = "Not indexed in this session";
    std::mutex time_cube_ui_mutex;
    std::thread time_cube_ui_worker;
    std::atomic<bool> time_cube_ui_running{false};
    std::atomic<bool> time_cube_ui_done{false};
    std::vector<bool> time_cube_selected(layers.size(), true);
    int time_cube_year_min = 2020;
    int time_cube_year_max = 2026;
    int time_cube_normalize_mode = 0; // 0=raw,1=index first year to 100,2=percent of max
    bool time_cube_show_excluded = false;
    json policy_hierarchy;
    bool policy_hierarchy_loaded = false;
    std::string policy_hierarchy_error;
    {
        std::ifstream in(root / "data" / "government" / "government_hierarchy_and_pay_2026.json");
        if (in) {
            try {
                in >> policy_hierarchy;
                policy_hierarchy_loaded = true;
            } catch (const std::exception& e) {
                policy_hierarchy_error = e.what();
            }
        } else {
            policy_hierarchy_error = "missing data/government/government_hierarchy_and_pay_2026.json";
        }
    }
    char policy_hierarchy_query[128] = "";
    int policy_hierarchy_scope = 0; // 0=all,1=maryland,2=baltimore,3=federal
    std::vector<PublicServantRosterRow> public_servant_roster;
    {
        std::ifstream in(root / "data" / "public_servants" / "normalized_public_servants.jsonl");
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            try {
                json row = json::parse(line);
                PublicServantRosterRow out;
                out.source_id = row.value("source_id", "");
                out.source_type = row.value("source_type", "");
                out.jurisdiction = row.value("jurisdiction", "");
                out.employer = row.value("employer", "");
                out.agency = row.value("agency", "");
                out.person_name = row.value("person_name", "");
                out.role_title = row.value("role_title", "");
                out.pay_grade = row.value("pay_grade", "");
                out.annual_salary = row.value("annual_salary", "");
                out.gross_pay = row.value("gross_pay", "");
                out.fiscal_year = row.value("fiscal_year", "");
                out.source_url = row.value("source_url", "");
                out.provenance_note = row.value("provenance_note", "");
                public_servant_roster.push_back(std::move(out));
            } catch (...) {
            }
        }
    }
    std::string people_pay_cached_query;
    int people_pay_cached_scope = -1;
    size_t people_pay_cache_matched_count = 0;
    std::vector<size_t> people_pay_visible_rows;
    size_t people_pay_cache_rebuilds = 0;
    size_t people_pay_rendered_rows_last = 0;
    PolicyVizNode policy_viz_root;
    std::string policy_viz_cached_query;
    int policy_viz_cached_scope = -1;
    int policy_viz_cached_metric = -1;
    int policy_viz_metric = 0; // 0=personnel count, 1=pay total
    size_t policy_viz_cache_rebuilds = 0;
    size_t policy_viz_node_count = 0;
    std::vector<LanPeerInfo> lan_peers;
    std::string lan_scan_status = "Not scanned";
    std::chrono::steady_clock::time_point lan_last_scan_at{};
    char arkavo_room_id[128] = "worldsim-default-room";
    std::string arkavo_status = "idle";
    std::string arkavo_err;
    std::unique_ptr<ArkavoRealtimeClient> arkavo_client;
    std::unique_ptr<ArkavoRtcSessionManager> arkavo_rtc;
    char arkavo_send_peer[160] = "";
    char arkavo_send_path[512] = "";

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        int fb_w = 0;
        int fb_h = 0;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        if (fb_w > 0 && fb_h > 0 && (fb_w != g_MainWindowData.Width || fb_h != g_MainWindowData.Height)) {
            w = fb_w;
            h = fb_h;
            g_SwapChainRebuild = true;
        }

        if (g_SwapChainRebuild) {
            glfwGetFramebufferSize(window, &w, &h);
            if (w > 0 && h > 0) {
                ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
                ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData, g_QueueFamily, g_Allocator, w, h, g_MinImageCount);
                g_MainWindowData.FrameIndex = 0;
                g_SwapChainRebuild = false;
            }
        }

        ImGui::SetCurrentContext(main_imgui_context);
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuiIO& frame_io = ImGui::GetIO();
        const FrameLayout frame_layout = updateFrameLayoutAndTextScale(
            frame_io,
            ui_left_panel_frac,
            ui_right_panel_frac,
            ui_text_scale);
        const float layout_w = frame_layout.layout_w;
        const float layout_h = frame_layout.layout_h;
        const float layout_margin = frame_layout.layout_margin;
        const float layout_gap = frame_layout.layout_gap;
        const float left_panel_w = frame_layout.left_panel_w;
        const float right_panel_w = frame_layout.right_panel_w;
        const float map_w = frame_layout.map_w;
        const float map_x = frame_layout.map_x;
        const float main_panel_h = frame_layout.main_panel_h;
        drainRetiredTextures();
        const auto prof_frame_begin = std::chrono::steady_clock::now();
        auto prof_ms_since = [](std::chrono::steady_clock::time_point t) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        };
        size_t prof_tiles_drawn_frame = 0;
        size_t prof_features_considered_frame = 0;
        size_t prof_features_drawn_frame = 0;
        bool layer_fill_state_changed = false;
        bool layer_hover_state_changed = false;
        bool layer_inspect_state_changed = false;
        bool layer_heatmap_state_changed = false;
        bool heatmap_settings_state_changed = false;
        bool heatmap_controls_active = false;
        FramePreludeResult frame_prelude = runFramePrelude(FramePreludeContext{
            &root,
            &layers,
            &layer_spatial,
            &layer_profile_dirty,
            &layer_profile_snapshot,
            &layer_profile_mutex,
            &layer_registry,
            &local_layer_exists_cache,
            &data_freshness_state,
            &data_freshness_msg,
            &data_library_status_msg,
            &layer_download_queue,
            &layer_download_inflight,
            &layer_download_active_idx,
            &layer_download_future,
            &layer_download_active_file,
            &layer_download_last_event,
            &layer_download_queue_loaded,
            &lan_peers,
            &lan_scan_status,
            &lan_last_scan_at,
            kProtocolVersion,
            &center_lon,
            &center_lat,
            &zoom,
            kMinZoom,
            kMaxZoom,
            &current_zoom_state,
            &current_lon_state,
            &current_lat_state,
            &api_layer_mutex,
            &api_layer_enable_cmds,
            &api_layer_fill_cmds,
            &api_layer_download_cmds,
            &layer_fill_mutex,
            &layer_fill_enabled,
            &layer_fill_state_changed,
            &api_ui_cmd_seq,
            &api_ui_cmd_kind,
            &api_ui_cmd_x,
            &api_ui_cmd_y,
            &api_ui_cmd_button,
            &api_ui_cmd_scroll_y,
            &api_ui_cmd_last_seq,
            &api_ui_mouse_release_pending,
            &api_ui_mouse_release_button,
            &api_zoom_cmd,
            &api_lon_cmd,
            &api_lat_cmd,
            &lazy_tile_download,
            &basemap_coverage_dirty,
            &basemap_availability_last_check,
            &osm_missing_tiles_cached,
            &osm_total_tiles_cached,
            &topo_missing_tiles_cached,
            &topo_total_tiles_cached,
            &basemap_source_has_any_files_cached,
            &topo_tiles_available_cached,
            &topo_vector_available_cached,
            kMinZoom,
            kMaxNativeTileZoom,
            kBasemapCoverageRefreshInterval,
            [&](size_t idx, bool exists) { mark_local_layer_exists(idx, exists); },
            [&](size_t idx, bool required) { enqueue_hydration(idx, required); },
            [&]() { refresh_local_layer_exists_cache(); }
        });
        const LeftPanelResult left_panel = drawLeftPanelWindow(LeftPanelContext{
            layout_margin,
            left_panel_w,
            main_panel_h,
            &root,
            &app_settings,
            &layers,
            &layer_registry,
            &local_layer_exists_cache,
            &data_freshness_state,
            &data_freshness_msg,
            &data_library_status_msg,
            &zoom,
            kMinZoom,
            kMaxZoom,
            &center_lon,
            &center_lat,
            &hover_inspector_mode,
            &hover_inspector_enabled,
            &show_sources_panel,
            &show_data_library,
            &parcel_parameter_mode,
            &layer_spatial,
            &layer_states,
            &status_mutex,
            &layer_fill_enabled,
            &layer_hover_enabled,
            &layer_inspect_enabled,
            &layer_heatmap_enabled,
            &layer_heatmap_algo,
            &layer_heatmap_max_zoom,
            &layer_parcel_detail_min_zoom,
            &layer_normalize_mode,
            &layer_heatmap_cell_px,
            &layer_heatmap_bandwidth_px,
            &layer_heatmap_blur_sigma_px,
            &layer_heatmap_percentile_clip,
            &layer_heatmap_multires_blend,
            &layer_heatmap_zoom_adaptive_bandwidth,
            &layer_heatmap_multires_enabled,
            &layer_heatmap_use_gradient,
            heatmap_algo,
            &heatmap_allow_cpu_fallback,
            &layer_fill_mutex,
            &layer_fill_state_changed,
            &layer_hover_state_changed,
            &layer_inspect_state_changed,
            &layer_heatmap_state_changed,
            &heatmap_controls_active,
            parcel_layer_idx,
            crime_nibrs_layer_idx,
            crime_legacy_layer_idx,
            zoning_layer_idx,
            &crime_filter_enabled,
            &crime_filter_use_year,
            &crime_year_min,
            &crime_year_max,
            &crime_filter_homicide,
            &crime_filter_robbery,
            &crime_filter_assault,
            &crime_filter_burglary,
            &crime_filter_theft,
            &crime_filter_auto_theft,
            &crime_filter_drug,
            &crime_filter_shooting,
            &crime_breakdown,
            &parcel_jurisdiction_filter,
            &parcel_jurisdiction_filter_dirty,
            &parcel_jurisdiction_filter_status,
            parcel_jurisdiction_options.data(),
            parcel_jurisdiction_options.size(),
            &basemap_download,
            &lazy_tile_download,
            &basemap_coverage_dirty,
            osm_missing_tiles_cached,
            osm_total_tiles_cached,
            topo_missing_tiles_cached,
            topo_total_tiles_cached,
            topo_tiles_available_cached,
            topo_vector_available_cached,
            kMaxNativeTileZoom,
            kMaxSatelliteNativeTileZoom,
            &zoning_zone_enabled,
            &zoning_zone_color,
            &zoning_zone_label,
            &zoning_metadata,
            &zoning_zone_order,
            &zoning_zone_counts,
            &zoning_group_zones,
            &zoning_group_order,
            [&](size_t i) { return enqueueLayerDownloadRequest(frame_prelude.layer_download, i); },
            [&](size_t i) { return layerDownloadPending(frame_prelude.layer_download, i); },
            [&]() { return frame_prelude.queue_all_missing_layer_downloads(); },
            [&](size_t i, bool exists) { mark_local_layer_exists(i, exists); },
            [&](size_t i, bool required) { enqueue_hydration(i, required); }
        });
        bool zoning_filters_changed = left_panel.zoning_filters_changed;
        const size_t downloadable_missing_layer_count = left_panel.downloadable_missing_layer_count;
        const size_t queueable_missing_layer_count = left_panel.queueable_missing_layer_count;
        auto resolve_download_label = [&](const std::string& key) -> std::string {
            return resolveDataLibraryDownloadLabel(layer_registry, layers, key);
        };

        PipelineProgressContext pipeline_progress_ctx;
        pipeline_progress_ctx.layer_count = layers.size();
        pipeline_progress_ctx.hydrated_count = &hydrated_count;
        pipeline_progress_ctx.triangulated_count = &triangulated_count;
        pipeline_progress_ctx.last_hydrated_seen = &last_hydrated_seen;
        pipeline_progress_ctx.last_triangulated_seen = &last_triangulated_seen;
        pipeline_progress_ctx.last_hydration_progress_at = &last_hydration_progress_at;
        pipeline_progress_ctx.last_tri_progress_at = &last_tri_progress_at;
        pipeline_progress_ctx.hydration_started_at = hydration_started_at;
        pipeline_progress_ctx.hydrated_mutex = &hydrated_mutex;
        pipeline_progress_ctx.hydrated_queue = &hydrated_queue;
        pipeline_progress_ctx.tri_mutex = &tri_mutex;
        pipeline_progress_ctx.tri_jobs = &tri_jobs;
        const PipelineProgressSnapshot pipeline_progress = updatePipelineProgress(pipeline_progress_ctx);
        const size_t hydrated_now = pipeline_progress.hydrated_now;
        const size_t triangulated_now = pipeline_progress.triangulated_now;
        const size_t hydrated_pending = pipeline_progress.hydrated_pending;
        const size_t tri_pending = pipeline_progress.tri_pending;
        const float hydrated_frac = pipeline_progress.hydrated_frac;
        const float tri_frac = pipeline_progress.tri_frac;
        const double elapsed_s = pipeline_progress.elapsed_s;
        const double hydrate_idle_s = pipeline_progress.hydrate_idle_s;
        const double tri_idle_s = pipeline_progress.tri_idle_s;

        drawGearPanel(&show_sources_panel, root, bootstrap);
        DataLibraryCoordinatorContext data_library_ctx;
        data_library_ctx.root = root;
        data_library_ctx.layers = &layers;
        data_library_ctx.layer_registry = &layer_registry;
        data_library_ctx.local_layer_exists_cache = &local_layer_exists_cache;
        data_library_ctx.data_freshness_state = &data_freshness_state;
        data_library_ctx.data_freshness_msg = &data_freshness_msg;
        data_library_ctx.data_library_status_msg = &data_library_status_msg;
        data_library_ctx.data_library_bulk_inflight = &data_library_bulk_inflight;
        data_library_ctx.data_library_bulk_mutex = &data_library_bulk_mutex;
        data_library_ctx.data_library_bulk_progress = &data_library_bulk_progress;
        data_library_ctx.data_library_bulk_future = &data_library_bulk_future;
        data_library_ctx.refresh_local_layer_exists_cache = [&]() { refresh_local_layer_exists_cache(); };
        data_library_ctx.enqueue_hydration = [&](size_t idx, bool required) { enqueue_hydration(idx, required); };
        DataLibraryUiContext data_library_ui_ctx;
        data_library_ui_ctx.root = &root;
        data_library_ui_ctx.layers = &layers;
        data_library_ui_ctx.coordinator = &data_library_ctx;
        data_library_ui_ctx.show_data_library = &show_data_library;
        data_library_ui_ctx.query_buffer = data_library_query;
        data_library_ui_ctx.query_buffer_size = sizeof(data_library_query);
        data_library_ui_ctx.download_phase = &data_library_download_phase;
        data_library_ui_ctx.include_large = &data_library_include_large;
        data_library_ui_ctx.cached_query = &data_library_cached_query;
        data_library_ui_ctx.cached_layer_count = &data_library_cached_layer_count;
        data_library_ui_ctx.visible_rows = &data_library_visible_rows;
        data_library_ui_ctx.cache_rebuilds = &data_library_cache_rebuilds;
        data_library_ui_ctx.rendered_rows_last = &data_library_rendered_rows_last;
        data_library_ui_ctx.enqueue_layer_download_request = [&](size_t idx) {
            return enqueueLayerDownloadRequest(frame_prelude.layer_download, idx);
        };
        data_library_ui_ctx.queue_all_missing_layer_downloads = [&]() {
            return frame_prelude.queue_all_missing_layer_downloads();
        };
        data_library_ui_ctx.downloadable_missing_layer_count = downloadable_missing_layer_count;
        data_library_ui_ctx.queueable_missing_layer_count = queueable_missing_layer_count;
        drawDataLibraryWindow(data_library_ui_ctx);
        runPerformanceRuntimeSupport(PerformanceRuntimeContext{
            layout_margin,
            layout_h,
            main_panel_h,
            layout_gap,
            left_panel_w,
            layers.size(),
            hydrated_now,
            triangulated_now,
            hydrated_pending,
            tri_pending,
            hydrated_frac,
            tri_frac,
            elapsed_s,
            hydrate_idle_s,
            tri_idle_s,
            &perf_frame_ms_avg,
            &perf_frame_ms_last,
            &perf_fps_avg,
            data_library_rendered_rows_last,
            data_library_visible_rows.size(),
            data_library_cache_rebuilds,
            people_pay_rendered_rows_last,
            people_pay_visible_rows.size(),
            people_pay_cache_rebuilds,
            &render_fill_success_last_frame,
            &render_fill_attempts_last_frame,
            &render_fill_no_triangles_last_frame,
            &render_fill_bad_indices_last_frame,
            &frame_prelude.lan_discovery,
            &lan_peers,
            &lan_scan_status,
            arkavo_room_id,
            sizeof(arkavo_room_id),
            &arkavo_status,
            &arkavo_err,
            &arkavo_client,
            &arkavo_rtc,
            arkavo_send_peer,
            sizeof(arkavo_send_peer),
            arkavo_send_path,
            sizeof(arkavo_send_path),
            &clear_cache_all,
            &clear_cache_hydration,
            &clear_cache_triangulation,
            &clear_cache_derived,
            &clear_cache_heatmap_memory,
            &clear_cache_heatmap_disk,
            &clear_cache_tile_memory,
            &clear_cache_tile_disk_presence,
            &last_cache_clear_msg,
            &cache_hydration_dir,
            &cache_triangulation_dir,
            &cache_derived_dir,
            &cache_aggregate_dir,
            &layers,
            &layer_spatial,
            &layer_profile_dirty,
            &layer_states,
            &hydrated_mutex,
            &hydrated_queue,
            &tri_mutex,
            &tri_jobs,
            &tri_results,
            &tri_cv,
            &hydrate_req_mutex,
            &hydrate_requests,
            &hydration_requested,
            &hydration_required,
            &status_mutex,
            parcel_layer_idx,
            vacant_notice_layer_idx,
            vacant_rehab_layer_idx,
            &hydrated_count,
            &triangulated_count,
            [&](size_t idx, bool required) { enqueue_hydration(idx, required); },
            [&](const fs::path& p) { return clear_cache_tree(p); },
            [&]() { clear_heatmap_runtime_cache(); },
            [&]() { reset_derived_cache_state(); },
            [&]() { trimProcessHeap(); },
            [&]() { clearTileDiskPresenceCache(); }
        });
        const LayerUiStateSyncResult ui_state_sync = syncLayerUiState(LayerUiStateSyncContext{
            &root,
            &layers,
            hover_inspector_mode,
            hover_inspector_enabled,
            &last_hover_inspector_mode,
            &last_enabled_state,
            zoning_filters_changed,
            layer_fill_state_changed,
            layer_hover_state_changed,
            layer_inspect_state_changed,
            layer_heatmap_state_changed,
            heatmap_settings_state_changed,
            &layer_profile_dirty,
            [&](size_t i) { return layer_data_available(i); },
            [&](size_t idx, bool required) { enqueue_hydration(idx, required); },
            &layer_states,
            &status_mutex,
            parcel_layer_idx,
            real_property_layer_idx,
            vacant_notice_layer_idx,
            vacant_rehab_layer_idx,
            tax_lien_layer_idx,
            tax_sale_layer_idx,
            zoning_layer_idx,
            filter_enabled,
            filter_owner,
            filter_address,
            filter_zip,
            &zoning_zone_enabled,
            &layer_fill_enabled,
            &layer_hover_enabled,
            &layer_inspect_enabled,
            &layer_heatmap_enabled,
            &layer_heatmap_max_zoom,
            &layer_parcel_detail_min_zoom,
            &layer_heatmap_use_gradient,
            &layer_choropleth_gamma,
            &layer_heatmap_algo,
            &layer_normalize_mode,
            &layer_heatmap_cell_px,
            &layer_heatmap_bandwidth_px,
            &layer_heatmap_blur_sigma_px,
            &layer_heatmap_percentile_clip,
            &layer_heatmap_zoom_adaptive_bandwidth,
            &layer_heatmap_multires_enabled,
            &layer_heatmap_multires_blend,
            &heatmap_algo,
            &heatmap_quality_preset,
            &global_heat_cell_px,
            &heatmap_bandwidth_px,
            &heatmap_blur_sigma_px,
            &heatmap_percentile_clip,
            &heatmap_zoom_adaptive_bandwidth,
            &heatmap_multires_enabled,
            &heatmap_multires_blend,
            &heatmap_allow_cpu_fallback,
            filter_use_date,
            filter_year_min,
            filter_year_max,
            filter_blocklot,
            filter_status,
            crime_filter_enabled,
            crime_filter_homicide,
            crime_filter_robbery,
            crime_filter_assault,
            crime_filter_burglary,
            crime_filter_theft,
            crime_filter_auto_theft,
            crime_filter_drug,
            crime_filter_shooting,
            crime_filter_use_year,
            crime_year_min,
            crime_year_max,
            owner_search_query,
            &selected_owners
        });
        const bool vacant_layer_active = ui_state_sync.vacant_layer_active;
        LayerPipelineDrainContext pipeline_drain_ctx;
        pipeline_drain_ctx.layers = &layers;
        pipeline_drain_ctx.hydrated_queue = &hydrated_queue;
        pipeline_drain_ctx.hydrated_mutex = &hydrated_mutex;
        pipeline_drain_ctx.tri_jobs = &tri_jobs;
        pipeline_drain_ctx.tri_results = &tri_results;
        pipeline_drain_ctx.tri_mutex = &tri_mutex;
        pipeline_drain_ctx.tri_cv = &tri_cv;
        pipeline_drain_ctx.layer_states = &layer_states;
        pipeline_drain_ctx.status_mutex = &status_mutex;
        pipeline_drain_ctx.hydration_requested = &hydration_requested;
        pipeline_drain_ctx.hydrate_req_mutex = &hydrate_req_mutex;
        pipeline_drain_ctx.layer_profile_dirty = &layer_profile_dirty;
        pipeline_drain_ctx.hydrated_count = &hydrated_count;
        pipeline_drain_ctx.triangulated_count = &triangulated_count;
        pipeline_drain_ctx.parcel_layer_idx = parcel_layer_idx;
        pipeline_drain_ctx.vacant_layer_active = vacant_layer_active;
        pipeline_drain_ctx.trim_process_heap = [&]() { trimProcessHeap(); };
        drainHydratedLayerQueue(pipeline_drain_ctx);
        drainTriangulationResults(pipeline_drain_ctx);

        DerivedLayerCachesContext derived_layer_caches_ctx;
        derived_layer_caches_ctx.root = &root;
        derived_layer_caches_ctx.layers = &layers;
        derived_layer_caches_ctx.app_settings = &app_settings;
        derived_layer_caches_ctx.zoning_layer_idx = zoning_layer_idx;
        derived_layer_caches_ctx.real_property_layer_idx = real_property_layer_idx;
        derived_layer_caches_ctx.vacant_notice_layer_idx = vacant_notice_layer_idx;
        derived_layer_caches_ctx.vacant_rehab_layer_idx = vacant_rehab_layer_idx;
        derived_layer_caches_ctx.tax_lien_layer_idx = tax_lien_layer_idx;
        derived_layer_caches_ctx.tax_sale_layer_idx = tax_sale_layer_idx;
        derived_layer_caches_ctx.parcel_layer_idx = parcel_layer_idx;
        derived_layer_caches_ctx.zoning_metadata = &zoning_metadata;
        derived_layer_caches_ctx.zoning_zone_enabled = &zoning_zone_enabled;
        derived_layer_caches_ctx.zoning_zone_color = &zoning_zone_color;
        derived_layer_caches_ctx.zoning_zone_label = &zoning_zone_label;
        derived_layer_caches_ctx.zoning_zone_order = &zoning_zone_order;
        derived_layer_caches_ctx.zoning_zone_counts = &zoning_zone_counts;
        derived_layer_caches_ctx.zoning_group_zones = &zoning_group_zones;
        derived_layer_caches_ctx.zoning_group_order = &zoning_group_order;
        derived_layer_caches_ctx.zoning_zone_discovered_feature_count = &zoning_zone_discovered_feature_count;
        derived_layer_caches_ctx.real_property_by_blocklot = &real_property_by_blocklot;
        derived_layer_caches_ctx.cached_real_property_size = &cached_real_property_size;
        derived_layer_caches_ctx.cached_vac_notice_size = &cached_vac_notice_size;
        derived_layer_caches_ctx.cached_vac_rehab_size = &cached_vac_rehab_size;
        derived_layer_caches_ctx.cached_tax_lien_size = &cached_tax_lien_size;
        derived_layer_caches_ctx.cached_tax_sale_size = &cached_tax_sale_size;
        derived_layer_caches_ctx.vacant_notice_count_by_blocklot = &vacant_notice_count_by_blocklot;
        derived_layer_caches_ctx.vacant_rehab_count_by_blocklot = &vacant_rehab_count_by_blocklot;
        derived_layer_caches_ctx.tax_lien_count_by_blocklot = &tax_lien_count_by_blocklot;
        derived_layer_caches_ctx.tax_lien_amount_by_blocklot = &tax_lien_amount_by_blocklot;
        derived_layer_caches_ctx.tax_sale_count_by_blocklot = &tax_sale_count_by_blocklot;
        derived_layer_caches_ctx.tax_sale_amount_by_blocklot = &tax_sale_amount_by_blocklot;
        derived_layer_caches_ctx.vacancy_maps_generation = &vacancy_maps_generation;
        derived_layer_caches_ctx.parcel_vacancy_generation_applied = &parcel_vacancy_generation_applied;
        derived_layer_caches_ctx.tax_maps_generation = &tax_maps_generation;
        derived_layer_caches_ctx.parcel_tax_generation_applied = &parcel_tax_generation_applied;
        derived_layer_caches_ctx.parcel_vac_notice_by_feature = &parcel_vac_notice_by_feature;
        derived_layer_caches_ctx.parcel_vac_rehab_by_feature = &parcel_vac_rehab_by_feature;
        derived_layer_caches_ctx.parcel_tax_lien_by_feature = &parcel_tax_lien_by_feature;
        derived_layer_caches_ctx.parcel_tax_sale_by_feature = &parcel_tax_sale_by_feature;
        derived_layer_caches_ctx.parcel_tax_lien_amount_by_feature = &parcel_tax_lien_amount_by_feature;
        derived_layer_caches_ctx.parcel_tax_sale_amount_by_feature = &parcel_tax_sale_amount_by_feature;
        derived_layer_caches_ctx.vacant_notice_rows_matched_total = &vacant_notice_rows_matched_total;
        derived_layer_caches_ctx.vacant_rehab_rows_matched_total = &vacant_rehab_rows_matched_total;
        derived_layer_caches_ctx.vacant_parcels_matched_total = &vacant_parcels_matched_total;
        derived_layer_caches_ctx.vacant_parcels_with_geometry_total = &vacant_parcels_with_geometry_total;
        derived_layer_caches_ctx.vacant_parcels_triangulated_renderable_total = &vacant_parcels_triangulated_renderable_total;
        derived_layer_caches_ctx.unified_parcels = &unified_parcels;
        derived_layer_caches_ctx.unified_parcel_cached_size = &unified_parcel_cached_size;
        derived_layer_caches_ctx.unified_real_property_cached_size = &unified_real_property_cached_size;
        derived_layer_caches_ctx.unified_vacancy_generation_applied = &unified_vacancy_generation_applied;
        derived_layer_caches_ctx.unified_tax_generation_applied = &unified_tax_generation_applied;
        derived_layer_caches_ctx.owner_aggregates_dirty = &owner_aggregates_dirty;
        refreshDerivedLayerCaches(derived_layer_caches_ctx);
        for (size_t li = 0; li < layers.size(); ++li) {
            LayerPipelineStatus st = LayerPipelineStatus::Queued;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if (li < layer_states.size()) st = layer_states[li].status;
            }
            const bool stable =
                st == LayerPipelineStatus::Hydrated ||
                st == LayerPipelineStatus::TriQueued ||
                st == LayerPipelineStatus::Triangulating ||
                st == LayerPipelineStatus::Ready;
            if (!stable) continue;
            if (!layer_spatial[li].built || layer_spatial[li].feature_count_built != layers[li].features.size()) {
                buildLayerSpatialIndex(layers[li], layer_spatial[li]);
                if (li < layer_profile_dirty.size()) layer_profile_dirty[li] = true;
            }
        }
        refreshLayerProfileSnapshot(frame_prelude.layer_profile_snapshot);

        auto real_property_for_parcel = [&](const LayerDef::FeatureGeom& parcel) -> const LayerDef::FeatureGeom* {
            if (real_property_layer_idx < 0 || (size_t)real_property_layer_idx >= layers.size()) return nullptr;
            std::string blocklot = featureBlockLotJoinKey(parcel);
            if (blocklot.empty()) return nullptr;
            auto itrp = real_property_by_blocklot.find(blocklot);
            if (itrp == real_property_by_blocklot.end()) return nullptr;
            const auto& rp_layer = layers[(size_t)real_property_layer_idx];
            if (itrp->second >= rp_layer.features.size()) return nullptr;
            return &rp_layer.features[itrp->second];
        };
        drawRightPanelWindow(RightPanelContext{
            &root,
            &duckdb_analytics,
            layout_w,
            right_panel_w,
            layout_margin,
            main_panel_h,
            &layers,
            &unified_parcels,
            &map_filter_state,
            &query_layers,
            &zoning_metadata,
            &real_property_by_blocklot,
            &selected_owners,
            &selected_parcel_index_set,
            &selected_parcel_indices,
            &parcel_selection,
            &element_info_state,
            &show_selected_parcel_details,
            &show_selected_zone_details,
            &selected_zone_idx,
            &center_lon,
            &center_lat,
            &zoom,
            parcel_layer_idx,
            zoning_layer_idx,
            real_property_layer_idx,
            crime_nibrs_layer_idx,
            crime_legacy_layer_idx,
            vacant_notice_layer_idx,
            vacant_rehab_layer_idx,
            tax_lien_layer_idx,
            tax_sale_layer_idx,
            parcel_parameter_mode,
            heatmap_algo,
            &layer_heatmap_enabled,
            &layer_heatmap_max_zoom,
            &layer_parcel_detail_min_zoom,
            &layer_heatmap_algo,
            &layer_heatmap_percentile_clip,
            &layer_choropleth_gamma,
            &layer_heatmap_state_changed,
            heatmap_percentile_clip,
            &parcel_vac_notice_by_feature,
            &parcel_vac_rehab_by_feature,
            &parcel_jurisdiction_result_set,
            &owner_class_overrides,
            &owner_class_overrides_loaded,
            &owner_class_overrides_dirty,
            &owner_aggregates,
            &filtered_aggregate_snapshot,
            &owner_aggregates_dirty,
            &owner_sort_mode,
            &owner_sorted_mode,
            &owner_class_filter_mode,
            &owner_class_assign_mode,
            &selected_owner_anchor,
            &owner_cached_parcel_size,
            &owner_cached_real_property_size,
            parcel_vacancy_generation_applied,
            parcel_tax_generation_applied,
            &prof_owner_ms_last,
            owner_search_query,
            sizeof(owner_search_query),
            &address_locate_status,
            &address_locate_matches,
            &record_year_hist,
            &record_year_hist_plot,
            &hist_feature_counts,
            &hist_enabled,
            &hist_dirty,
            &record_year_hist_max_bin,
            &record_year_nonzero_min,
            &record_year_nonzero_max,
            &record_year_nonzero_total,
            &selected_record_year,
            &selected_record_year_dirty,
            &selected_record_year_total,
            &selected_record_year_samples,
            (size_t)cached_vac_notice_size,
            (size_t)cached_vac_rehab_size,
            &vacant_notice_rows_matched_total,
            &vacant_rehab_rows_matched_total,
            &vacant_parcels_matched_total,
            &vacant_parcels_with_geometry_total,
            &duckdb_auto_rebuild_checked,
            &hydrate_req_mutex,
            &hydrate_requests,
            &hydration_requested,
            &hydrated_mutex,
            &hydrated_queue,
            real_property_for_parcel
        });

        drawMapTabWindow(MapTabContext{
            map_x,
            map_w,
            layout_margin,
            main_panel_h,
            &root,
            &app_settings,
            &duckdb_analytics,
            &center_lon,
            &center_lat,
            &zoom,
            kMinZoom,
            kMaxZoom,
            kMaxInternalMathZoom,
            &layers,
            &layer_spatial,
            &map_filter_state,
            &query_layers,
            &real_property_by_blocklot,
            &zoning_metadata,
            &zoning_zone_enabled,
            &zoning_zone_color,
            &parcel_selection,
            &selected_parcel_indices,
            &show_selected_zone_details,
            &selected_zone_idx,
            &element_info_state,
            real_property_layer_idx,
            parcel_layer_idx,
            zoning_layer_idx,
            crime_nibrs_layer_idx,
            crime_legacy_layer_idx,
            vacant_notice_layer_idx,
            vacant_rehab_layer_idx,
            tax_lien_layer_idx,
            tax_sale_layer_idx,
            parcel_parameter_mode,
            &layer_fill_enabled,
            &layer_hover_enabled,
            &layer_inspect_enabled,
            &layer_heatmap_enabled,
            &layer_heatmap_algo,
            &layer_heatmap_max_zoom,
            &layer_parcel_detail_min_zoom,
            &layer_heatmap_cell_px,
            &layer_heatmap_bandwidth_px,
            &layer_heatmap_blur_sigma_px,
            &layer_heatmap_percentile_clip,
            &layer_heatmap_zoom_adaptive_bandwidth,
            &layer_heatmap_multires_enabled,
            &layer_heatmap_multires_blend,
            &layer_heatmap_use_gradient,
            &layer_choropleth_gamma,
            &layer_normalize_mode,
            &parcel_jurisdiction_filter,
            parcel_jurisdiction_options.size(),
            &parcel_jurisdiction_filter_dirty,
            &parcel_jurisdiction_result_set,
            &parcel_jurisdiction_filter_status,
            &parcel_vac_notice_by_feature,
            &parcel_vac_rehab_by_feature,
            &parcel_tax_lien_by_feature,
            &parcel_tax_sale_by_feature,
            &parcel_tax_lien_amount_by_feature,
            &parcel_tax_sale_amount_by_feature,
            &unified_parcels,
            global_heat_cell_px,
            heatmap_algo,
            heatmap_quality_preset,
            heatmap_bandwidth_px,
            heatmap_blur_sigma_px,
            heatmap_percentile_clip,
            heatmap_zoom_adaptive_bandwidth,
            heatmap_multires_enabled,
            heatmap_multires_blend,
            heatmap_allow_cpu_fallback,
            heatmap_controls_active,
            &heatmap_runtime,
            hover_inspector_mode,
            &hover_inspector_enabled,
            &lazy_tile_download,
            &topo_tiles_available_cached,
            &topo_vector_available_cached,
            &basemap_source_has_any_files_cached,
            &tile_root_dir_cached,
            &basemap_availability_last_check,
            kMaxNativeTileZoom,
            &prof_tiles_drawn_frame,
            &prof_features_considered_frame,
            &prof_features_drawn_frame,
            &prof_tile_ms_last,
            &prof_layer_ms_last,
            &prof_heatmap_ms_last,
            &prof_heat_samples_last,
            &prof_heatmap_gpu_splat_active,
            &prof_heatmap_high_quality,
            &prof_heatmap_cache_valid,
            &prof_heatmap_texture_resident,
            &prof_heatmap_async_inflight,
            &prof_heatmap_cache_key,
            &prof_heatmap_texture_cache_entries,
            &visible_vacant_parcels_last_frame,
            &prof_overlay_ms_last,
            &render_fill_attempts_last_frame,
            &render_fill_success_last_frame,
            &render_fill_no_triangles_last_frame,
            &render_fill_bad_indices_last_frame,
            &time_cube_service,
            &time_cube_ui_result,
            &time_cube_ui_loaded,
            &time_cube_ui_status,
            &time_cube_ui_mutex,
            &time_cube_ui_worker,
            &time_cube_ui_running,
            &time_cube_ui_done,
            &time_cube_selected,
            &time_cube_year_min,
            &time_cube_year_max,
            &time_cube_normalize_mode,
            &time_cube_show_excluded,
            &policy_hierarchy,
            policy_hierarchy_loaded,
            &policy_hierarchy_error,
            policy_hierarchy_query,
            sizeof(policy_hierarchy_query),
            &policy_hierarchy_scope,
            &public_servant_roster,
            &people_pay_cached_query,
            &people_pay_cached_scope,
            &people_pay_cache_matched_count,
            &people_pay_visible_rows,
            &people_pay_cache_rebuilds,
            &people_pay_rendered_rows_last,
            &policy_viz_root,
            &policy_viz_cached_query,
            &policy_viz_cached_scope,
            &policy_viz_cached_metric,
            &policy_viz_metric,
            &policy_viz_cache_rebuilds,
            &policy_viz_node_count,
            real_property_for_parcel
        });
        finalizeFrameSupport(FrameSupportFinalizationContext{
            wd,
            &last_frame_ts,
            &ema_frame_ms,
            kPerfAlpha,
            prof_frame_begin,
            prof_tiles_drawn_frame,
            prof_features_considered_frame,
            prof_features_drawn_frame,
            &perf_frame_ms_last,
            &perf_frame_ms_avg,
            &perf_fps_avg,
            &prof_ui_ms_last,
            &prof_owner_ms_last,
            &prof_tile_ms_last,
            &prof_layer_ms_last,
            &prof_heatmap_ms_last,
            &prof_overlay_ms_last,
            &prof_present_ms_last,
            &prof_tiles_drawn_last,
            &prof_features_considered_last,
            &prof_features_drawn_last,
            &prof_retired_textures,
            &prof_tile_cache_size,
            &prof_heat_samples_last,
            &profile_mutex,
            &profile_samples,
            &profile_sample_pos,
            &profile_sample_count
        });

        renderSecondaryDownloadQueueWindow(SecondaryDownloadQueueWindowContext{
            download_queue_window,
            &download_queue_swapchain_rebuild,
            &dq_w,
            &dq_h,
            download_queue_imgui_context,
            main_imgui_context,
            &download_queue_wd,
            ui_text_scale,
            &basemap_download,
            &root,
            &data_library_status_msg,
            resolve_download_label,
            &lazy_tile_download,
            layer_download_inflight,
            layer_download_active_idx,
            &layers,
            &layer_download_queue,
            &layer_download_last_event
        });
    }
    vkDeviceWaitIdle(g_Device);
    if (download_queue_imgui_context) {
        ImGui::SetCurrentContext(download_queue_imgui_context);
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext(download_queue_imgui_context);
        download_queue_imgui_context = nullptr;
    }
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &download_queue_wd, g_Allocator);
    if (download_queue_window) {
        glfwDestroyWindow(download_queue_window);
        download_queue_window = nullptr;
    }
    ImGui::SetCurrentContext(main_imgui_context);
    destroyHeatmapRuntimeTexturesNow(heatmap_runtime);
    AppShutdownContextFactoryInput shutdown_input;
    shutdown_input.root = &root;
    shutdown_input.app_settings = &app_settings;
    shutdown_input.window = window;
    shutdown_input.layers = &layers;
    shutdown_input.hover_inspector_enabled = hover_inspector_enabled;
    shutdown_input.hover_inspector_mode = &hover_inspector_mode;
    shutdown_input.zoning_zone_enabled = &zoning_zone_enabled;
    shutdown_input.layer_fill_enabled = &layer_fill_enabled;
    shutdown_input.layer_hover_enabled = &layer_hover_enabled;
    shutdown_input.layer_inspect_enabled = &layer_inspect_enabled;
    shutdown_input.layer_heatmap_enabled = &layer_heatmap_enabled;
    shutdown_input.layer_heatmap_max_zoom = &layer_heatmap_max_zoom;
    shutdown_input.layer_parcel_detail_min_zoom = &layer_parcel_detail_min_zoom;
    shutdown_input.layer_heatmap_use_gradient = &layer_heatmap_use_gradient;
    shutdown_input.layer_choropleth_gamma = &layer_choropleth_gamma;
    shutdown_input.layer_heatmap_algo = &layer_heatmap_algo;
    shutdown_input.layer_normalize_mode = &layer_normalize_mode;
    shutdown_input.layer_heatmap_cell_px = &layer_heatmap_cell_px;
    shutdown_input.layer_heatmap_bandwidth_px = &layer_heatmap_bandwidth_px;
    shutdown_input.layer_heatmap_blur_sigma_px = &layer_heatmap_blur_sigma_px;
    shutdown_input.layer_heatmap_percentile_clip = &layer_heatmap_percentile_clip;
    shutdown_input.layer_heatmap_zoom_adaptive_bandwidth = &layer_heatmap_zoom_adaptive_bandwidth;
    shutdown_input.layer_heatmap_multires_enabled = &layer_heatmap_multires_enabled;
    shutdown_input.layer_heatmap_multires_blend = &layer_heatmap_multires_blend;
    shutdown_input.heatmap_algo = &heatmap_algo;
    shutdown_input.heatmap_quality_preset = &heatmap_quality_preset;
    shutdown_input.global_heat_cell_px = &global_heat_cell_px;
    shutdown_input.heatmap_bandwidth_px = &heatmap_bandwidth_px;
    shutdown_input.heatmap_blur_sigma_px = &heatmap_blur_sigma_px;
    shutdown_input.heatmap_percentile_clip = &heatmap_percentile_clip;
    shutdown_input.heatmap_zoom_adaptive_bandwidth = &heatmap_zoom_adaptive_bandwidth;
    shutdown_input.heatmap_multires_enabled = &heatmap_multires_enabled;
    shutdown_input.heatmap_multires_blend = &heatmap_multires_blend;
    shutdown_input.heatmap_allow_cpu_fallback = &heatmap_allow_cpu_fallback;
    shutdown_input.filter_enabled = &filter_enabled;
    shutdown_input.filter_use_date = &filter_use_date;
    shutdown_input.filter_year_min = &filter_year_min;
    shutdown_input.filter_year_max = &filter_year_max;
    shutdown_input.filter_blocklot = filter_blocklot;
    shutdown_input.filter_status = filter_status;
    shutdown_input.filter_address = filter_address;
    shutdown_input.filter_owner = filter_owner;
    shutdown_input.filter_zip = filter_zip;
    shutdown_input.crime_filter_enabled = &crime_filter_enabled;
    shutdown_input.crime_filter_homicide = &crime_filter_homicide;
    shutdown_input.crime_filter_robbery = &crime_filter_robbery;
    shutdown_input.crime_filter_assault = &crime_filter_assault;
    shutdown_input.crime_filter_burglary = &crime_filter_burglary;
    shutdown_input.crime_filter_theft = &crime_filter_theft;
    shutdown_input.crime_filter_auto_theft = &crime_filter_auto_theft;
    shutdown_input.crime_filter_drug = &crime_filter_drug;
    shutdown_input.crime_filter_shooting = &crime_filter_shooting;
    shutdown_input.crime_filter_use_year = &crime_filter_use_year;
    shutdown_input.crime_year_min = &crime_year_min;
    shutdown_input.crime_year_max = &crime_year_max;
    shutdown_input.owner_search_query = owner_search_query;
    shutdown_input.selected_owners = &selected_owners;
    shutdown_input.center_lon = &center_lon;
    shutdown_input.center_lat = &center_lat;
    shutdown_input.zoom = &zoom;
    shutdown_input.selected_parcel_idx = &selected_parcel_idx;
    shutdown_input.selected_parcel_indices = &selected_parcel_indices;
    shutdown_input.hydration_stop = &hydration_stop;
    shutdown_input.time_cube_ui_worker = &time_cube_ui_worker;
    shutdown_input.hydrate_req_cv = &hydrate_req_cv;
    shutdown_input.tri_cv = &tri_cv;
    shutdown_input.hydration_workers = &hydration_workers;
    shutdown_input.triangulation_worker = &triangulation_worker;
    shutdown_input.status_api_worker = &status_api_worker;
    shutdown_input.dataset_api_worker = &dataset_api_worker;
    shutdown_input.lan_discovery_worker = &lan_discovery_worker;
    shutdown_input.heatmap_raster_texture = &heatmap_runtime.raster_texture;
    AppShutdownContext shutdown_ctx = makeAppShutdownContext(shutdown_input);
    shutdownWorldSimApp(shutdown_ctx);
    return 0;
}
