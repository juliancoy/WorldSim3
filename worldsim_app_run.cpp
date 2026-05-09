#include "worldsim_app_internal.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "layer_state_io.h"
#include "feature_props.h"
#include "filters.h"
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
#include "layer_runtime.h"
#include "layer_geometry.h"
#include "layer_workers.h"
#include "memory_utils.h"
#include "aggregate_visualization_strategies.h"
#include "map_render_heatmap_pass.h"
#include "map_render_hover.h"
#include "map_render_layers.h"
#include "map_render_overlays.h"
#include "map_render_projection.h"
#include "map_render_utils.h"
#include "parcel_timeline.h"
#include "parcel_unified.h"
#include "heatmap_render.h"
#include "time_cube_panel.h"
#include "policy_panel.h"
#include "model_tabs_panel.h"
#include "layer_settings.h"
#include "download_queue.h"
#include "duckdb_analytics.h"
#include "sql_tab.h"
#include "app_lifecycle.h"
#include "worldsim_app.h"
#include "app_utils.h"
#include "zoning.h"
#include "vacancy_overlay.h"
#include "status_api.h"
#include "dataset_lan_api.h"
#include "worldsim_cli.h"
#include "worldsim_dataset_bootstrap.h"
#include "worldsim_bootstrap.h"
#include "parcel_matched_layers.h"
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
    const WorldsimCliOptions cli_options = parseWorldsimCliOptions(argc, argv);
    const int cli_result = runWorldsimCliImmediate(root, cli_options);
    if (cli_result >= 0) return cli_result;

    AppSettings app_settings;
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

    auto layers = loadManifest(root);
    const WorldsimLayerIndices layer_indices = findWorldsimLayerIndices(layers);
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
    std::vector<int> layer_heatmap_algo(layers.size(), -1);
    std::vector<int> layer_normalize_mode(layers.size(), 0);
    std::vector<float> layer_heatmap_cell_px(layers.size(), 24.0f);
    std::vector<float> layer_heatmap_bandwidth_px(layers.size(), 18.0f);
    std::vector<float> layer_heatmap_blur_sigma_px(layers.size(), 6.0f);
    std::vector<float> layer_heatmap_percentile_clip(layers.size(), 95.0f);
    std::vector<bool> layer_heatmap_zoom_adaptive_bandwidth(layers.size(), true);
    std::vector<bool> layer_heatmap_multires_enabled(layers.size(), true);
    std::vector<float> layer_heatmap_multires_blend(layers.size(), 0.5f);
    // Heatmap runtime settings (loaded/saved via layer_ui_state.json)
    float global_heat_cell_px = 24.0f;
    int heatmap_algo = kAggregateGpuSplatBlur;
    int heatmap_quality_preset = 1; // 0=fast,1=balanced,2=high
    float heatmap_bandwidth_px = 18.0f;
    float heatmap_blur_sigma_px = 6.0f;
    float heatmap_percentile_clip = 95.0f;
    bool heatmap_zoom_adaptive_bandwidth = true;
    bool heatmap_multires_enabled = true;
    float heatmap_multires_blend = 0.5f;
    bool heatmap_allow_cpu_fallback = false;
    std::vector<CachedHeatCell> heatmap_cached_cells;
    TileTexture heatmap_raster_texture;
    bool heatmap_raster_texture_valid = false;
    HeatmapRaster heatmap_cached_raster_meta;
    uint64_t heatmap_raster_cache_key = 0;
    uint64_t heatmap_cache_key = 0;
    bool heatmap_cache_valid = false;
    std::future<std::pair<uint64_t, HeatmapRenderData>> heatmap_async_future;
    bool heatmap_async_inflight = false;
    uint64_t heatmap_pending_key = 0;
    struct CachedAggregateTexture {
        std::vector<CachedHeatCell> cells;
        HeatmapRaster raster;
        TileTexture texture;
        uint64_t last_used_frame = 0;
    };
    std::unordered_map<uint64_t, CachedAggregateTexture> heatmap_texture_cache;
    uint64_t heatmap_texture_cache_frame = 0;
    constexpr size_t kMaxHeatmapTextureCacheEntries = 8;
    auto prune_heatmap_texture_cache = [&]() {
        while (heatmap_texture_cache.size() > kMaxHeatmapTextureCacheEntries) {
            auto evict_it = heatmap_texture_cache.begin();
            for (auto it = heatmap_texture_cache.begin(); it != heatmap_texture_cache.end(); ++it) {
                if (it->second.last_used_frame < evict_it->second.last_used_frame) evict_it = it;
            }
            destroyTileTexture(evict_it->second.texture);
            heatmap_texture_cache.erase(evict_it);
        }
    };
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
    TimeCubeService time_cube_service(root);
    DuckDbAnalytics duckdb_analytics(root);
    std::mutex status_mutex;
    std::vector<LayerRuntimeState> layer_states(layers.size());
    std::vector<LayerSpatialIndex> layer_spatial(layers.size());
    std::vector<uint32_t> render_candidates;
    struct OwnerAggregate {
        std::string owner;
        std::string owner_class;
        size_t property_count = 0;
        double area_m2 = 0.0;
        double value_usd = 0.0;
    };
    std::vector<OwnerAggregate> owner_aggregates;
    MapFilterState map_filter_state;
    auto& selected_owners = map_filter_state.selected_owners;
    std::vector<QueryMapLayer> query_layers;
    std::unordered_map<std::string, std::string> owner_class_overrides;
    bool owner_class_overrides_loaded = false;
    bool owner_class_overrides_dirty = false;
    int owner_class_filter_mode = 0; // 0=all
    int owner_class_assign_mode = 1; // default government
    int selected_owner_anchor = -1;
    struct FilteredAggregateSnapshot {
        bool valid = false;
        uint64_t selection_key = 0;
        uint64_t data_key = 0;
        size_t vacancy_parcels_matched = 0;
        size_t vacancy_parcels_with_geometry = 0;
        size_t owner_property_count = 0;
        double owner_area_m2 = 0.0;
        double owner_value_usd = 0.0;
    };
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
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].enabled) {
            hydrate_requests.push_back(i);
            hydration_requested[i] = true;
        }
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

    std::thread status_api_worker = startStatusApiWorker(StatusApiContext{
        kAppVersion,
        kProtocolVersion,
        kMaxTileCache,
        &hydration_stop,
        &layers,
        &time_cube_service,
        &g_ScreenshotState,
        &status_mutex,
        &layer_states,
        &layer_fill_mutex,
        &layer_fill_enabled,
        &layer_hover_enabled,
        &layer_inspect_enabled,
        &layer_heatmap_enabled,
        &hydration_started_at,
        &hydrated_count,
        &triangulated_count,
        &prof_tile_cache_size,
        &current_zoom_state,
        &current_lon_state,
        &current_lat_state,
        &visible_vacant_parcels_last_frame,
        &vacant_parcels_matched_total,
        &vacant_parcels_with_geometry_total,
        &vacant_parcels_triangulated_renderable_total,
        &perf_frame_ms_avg,
        &perf_frame_ms_last,
        &perf_fps_avg,
        &ui_left_panel_frac,
        &ui_right_panel_frac,
        &render_fill_attempts_last_frame,
        &render_fill_success_last_frame,
        &render_fill_no_triangles_last_frame,
        &render_fill_bad_indices_last_frame,
        &api_layer_mutex,
        &api_layer_enable_cmds,
        &api_layer_fill_cmds,
        &api_layer_download_cmds,
        &api_zoom_cmd,
        &api_lon_cmd,
        &api_lat_cmd,
        &api_ui_cmd_seq,
        &api_ui_cmd_kind,
        &api_ui_cmd_x,
        &api_ui_cmd_y,
        &api_ui_cmd_button,
        &api_ui_cmd_scroll_y,
        &layer_profile_mutex,
        &layer_profile_snapshot,
        &profile_mutex,
        &profile_samples,
        &profile_sample_pos,
        &profile_sample_count,
        &profile_reset_generation,
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
        &prof_heat_samples_last,
        &prof_retired_textures,
        &prof_heatmap_gpu_splat_active,
        &prof_heatmap_high_quality,
        &prof_heatmap_cache_valid,
        &prof_heatmap_texture_resident,
        &prof_heatmap_async_inflight,
        &prof_heatmap_cache_key,
        &prof_heatmap_texture_cache_entries
    });

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
    std::string data_library_status_msg;
    std::string data_library_cached_query;
    size_t data_library_cached_layer_count = 0;
    std::vector<size_t> data_library_visible_rows;
    size_t data_library_cache_rebuilds = 0;
    size_t data_library_rendered_rows_last = 0;
    std::vector<FreshnessState> data_freshness_state(layers.size(), FreshnessState::Unknown);
    std::vector<std::string> data_freshness_msg(layers.size(), "");
    std::vector<bool> local_layer_exists_cache(layers.size(), false);
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
    int data_library_download_phase = 0;
    bool data_library_include_large = false;
    bool data_library_bulk_inflight = false;
    std::mutex data_library_bulk_mutex;
    std::string data_library_bulk_progress;
    std::future<LayerDownloadSummary> data_library_bulk_future;
    DownloadQueueState basemap_download;
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
    struct AddressLocateMatch {
        size_t parcel_idx = (size_t)-1;
        int score = 0;
        std::string address;
    };
    std::vector<AddressLocateMatch> address_locate_matches;
    auto& filter_owner = map_filter_state.owner;
    char owner_search_query[96] = "";
    std::string owner_info_owner;
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
    bool show_selected_parcel_details = false;
    size_t selected_parcel_idx = (size_t)-1;
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
    struct LanPeerInfo {
        std::string ip;
        std::string app_name;
        std::string app_version;
        int protocol_version = 0;
        int dataset_port = 0;
        bool protocol_match = false;
    };
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

#include "worldsim_app_run_loop_part1.inc"
#include "worldsim_app_run_loop_part2.inc"
#include "worldsim_app_run_loop_part3.inc"
#include "worldsim_app_run_loop_part4.inc"
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
    for (auto& kv : heatmap_texture_cache) destroyTileTextureNow(kv.second.texture);
    heatmap_texture_cache.clear();
    AppShutdownContext shutdown_ctx;
    shutdown_ctx.root = &root;
    shutdown_ctx.app_settings = &app_settings;
    shutdown_ctx.window = window;
    shutdown_ctx.layers = &layers;
    shutdown_ctx.hover_inspector_enabled = hover_inspector_enabled;
    shutdown_ctx.hover_inspector_mode = &hover_inspector_mode;
    shutdown_ctx.zoning_zone_enabled = &zoning_zone_enabled;
    shutdown_ctx.layer_fill_enabled = &layer_fill_enabled;
    shutdown_ctx.layer_hover_enabled = &layer_hover_enabled;
    shutdown_ctx.layer_inspect_enabled = &layer_inspect_enabled;
    shutdown_ctx.layer_heatmap_enabled = &layer_heatmap_enabled;
    shutdown_ctx.layer_heatmap_max_zoom = &layer_heatmap_max_zoom;
    shutdown_ctx.layer_parcel_detail_min_zoom = &layer_parcel_detail_min_zoom;
    shutdown_ctx.layer_heatmap_use_gradient = &layer_heatmap_use_gradient;
    shutdown_ctx.layer_heatmap_algo = &layer_heatmap_algo;
    shutdown_ctx.layer_normalize_mode = &layer_normalize_mode;
    shutdown_ctx.layer_heatmap_cell_px = &layer_heatmap_cell_px;
    shutdown_ctx.layer_heatmap_bandwidth_px = &layer_heatmap_bandwidth_px;
    shutdown_ctx.layer_heatmap_blur_sigma_px = &layer_heatmap_blur_sigma_px;
    shutdown_ctx.layer_heatmap_percentile_clip = &layer_heatmap_percentile_clip;
    shutdown_ctx.layer_heatmap_zoom_adaptive_bandwidth = &layer_heatmap_zoom_adaptive_bandwidth;
    shutdown_ctx.layer_heatmap_multires_enabled = &layer_heatmap_multires_enabled;
    shutdown_ctx.layer_heatmap_multires_blend = &layer_heatmap_multires_blend;
    shutdown_ctx.heatmap_algo = &heatmap_algo;
    shutdown_ctx.heatmap_quality_preset = &heatmap_quality_preset;
    shutdown_ctx.global_heat_cell_px = &global_heat_cell_px;
    shutdown_ctx.heatmap_bandwidth_px = &heatmap_bandwidth_px;
    shutdown_ctx.heatmap_blur_sigma_px = &heatmap_blur_sigma_px;
    shutdown_ctx.heatmap_percentile_clip = &heatmap_percentile_clip;
    shutdown_ctx.heatmap_zoom_adaptive_bandwidth = &heatmap_zoom_adaptive_bandwidth;
    shutdown_ctx.heatmap_multires_enabled = &heatmap_multires_enabled;
    shutdown_ctx.heatmap_multires_blend = &heatmap_multires_blend;
    shutdown_ctx.heatmap_allow_cpu_fallback = &heatmap_allow_cpu_fallback;
    shutdown_ctx.filter_enabled = &filter_enabled;
    shutdown_ctx.filter_use_date = &filter_use_date;
    shutdown_ctx.filter_year_min = &filter_year_min;
    shutdown_ctx.filter_year_max = &filter_year_max;
    shutdown_ctx.filter_blocklot = filter_blocklot;
    shutdown_ctx.filter_status = filter_status;
    shutdown_ctx.filter_address = filter_address;
    shutdown_ctx.filter_owner = filter_owner;
    shutdown_ctx.filter_zip = filter_zip;
    shutdown_ctx.crime_filter_enabled = &crime_filter_enabled;
    shutdown_ctx.crime_filter_homicide = &crime_filter_homicide;
    shutdown_ctx.crime_filter_robbery = &crime_filter_robbery;
    shutdown_ctx.crime_filter_assault = &crime_filter_assault;
    shutdown_ctx.crime_filter_burglary = &crime_filter_burglary;
    shutdown_ctx.crime_filter_theft = &crime_filter_theft;
    shutdown_ctx.crime_filter_auto_theft = &crime_filter_auto_theft;
    shutdown_ctx.crime_filter_drug = &crime_filter_drug;
    shutdown_ctx.crime_filter_shooting = &crime_filter_shooting;
    shutdown_ctx.crime_filter_use_year = &crime_filter_use_year;
    shutdown_ctx.crime_year_min = &crime_year_min;
    shutdown_ctx.crime_year_max = &crime_year_max;
    shutdown_ctx.owner_search_query = owner_search_query;
    shutdown_ctx.selected_owners = &selected_owners;
    shutdown_ctx.hydration_stop = &hydration_stop;
    shutdown_ctx.time_cube_ui_worker = &time_cube_ui_worker;
    shutdown_ctx.hydrate_req_cv = &hydrate_req_cv;
    shutdown_ctx.tri_cv = &tri_cv;
    shutdown_ctx.hydration_workers = &hydration_workers;
    shutdown_ctx.triangulation_worker = &triangulation_worker;
    shutdown_ctx.status_api_worker = &status_api_worker;
    shutdown_ctx.dataset_api_worker = &dataset_api_worker;
    shutdown_ctx.lan_discovery_worker = &lan_discovery_worker;
    shutdown_ctx.heatmap_raster_texture = &heatmap_raster_texture;
    shutdownWorldSimApp(shutdown_ctx);
    return 0;
}
