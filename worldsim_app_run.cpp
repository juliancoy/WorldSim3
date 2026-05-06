#include "worldsim_app_internal.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "layer_state_io.h"
#include "feature_props.h"
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
#include "heatmap_render.h"
#include "time_cube_panel.h"
#include "policy_panel.h"
#include "model_tabs_panel.h"
#include "app_lifecycle.h"
#include "worldsim_app.h"
#include "app_utils.h"
#include "zoning.h"
#include "vacancy_overlay.h"
#include "status_api.h"
#include "dataset_lan_api.h"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <condition_variable>
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
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "earcut.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
int runLayerDownloadCli(const fs::path& root, const std::string& phase, bool include_large) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::cout << "Using app root: " << root.string() << "\n";
    std::cout << "Using manifest phase: " << (phase.empty() ? "all" : phase) << "\n";
    LayerDownloadSummary summary = downloadLayerManifestPhase(
        root,
        phase.empty() ? "all" : phase,
        include_large,
        [](size_t i, size_t total, const std::string& msg) {
            std::cout << "[" << i << "/" << total << "] " << msg << "\n";
        });
    curl_global_cleanup();
    std::cout << "Done. downloaded=" << summary.downloaded
              << " skipped=" << summary.skipped
              << " failed=" << summary.failed
              << " total=" << summary.total << "\n";
    return summary.failed == 0 ? 0 : 1;
}

bool envEnabled(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) return false;
    std::string s = toLowerAscii(value);
    return s != "0" && s != "false" && s != "no" && s != "off";
}
}

int runWorldSim3App(int argc, char** argv) {

    fs::path root = resolveAppRoot(fs::current_path(), argc > 0 ? argv[0] : nullptr);
    std::string download_phase;
    bool download_layers_cli = false;
    bool include_large_downloads = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--vacancy-selftest") {
            return runVacancySelftest(root);
        }
        if (arg == "--download-layers") {
            download_layers_cli = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                download_phase = argv[++i];
            } else {
                download_phase = "all";
            }
        } else if (arg.rfind("--download-layers=", 0) == 0) {
            download_layers_cli = true;
            download_phase = arg.substr(std::strlen("--download-layers="));
        } else if (arg == "--include-large") {
            include_large_downloads = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: worldsim3 [--download-layers [all|must-have|nice-to-have|heavy-data|capital-flows]] [--include-large]\n"
                << "       worldsim3 --vacancy-selftest\n";
            return 0;
        }
    }
    if (download_layers_cli) {
        return runLayerDownloadCli(root, download_phase.empty() ? "all" : download_phase, include_large_downloads);
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (envEnabled("WORLD_SIM3_PRELOAD_DATA")) {
        const char* phase_env = std::getenv("WORLD_SIM3_PRELOAD_PHASE");
        const std::string preload_phase = (phase_env && *phase_env) ? std::string(phase_env) : "all";
        LayerDownloadSummary summary = downloadLayerManifestPhase(
            root,
            preload_phase,
            envEnabled("WORLD_SIM3_INCLUDE_LARGE"),
            [](size_t i, size_t total, const std::string& msg) {
                std::cout << "[preload " << i << "/" << total << "] " << msg << "\n";
            });
        if (summary.failed > 0) {
            std::cerr << "Preload completed with " << summary.failed << " failed downloads.\n";
        }
    }

    AppSettings app_settings;
    app_settings.vulkan_validation_enabled = g_EnableValidationLayers;
    app_settings = loadAppSettings(root, app_settings);
    g_EnableValidationLayers = app_settings.vulkan_validation_enabled;
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1600, 1000, "Baltimore Vulkan Map", nullptr, nullptr);

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

    BootstrapProgress bootstrap;
    bootstrap.running.store(false, std::memory_order_relaxed);
    bootstrap.done.store(true, std::memory_order_relaxed);
    bootstrap.phase.store(3, std::memory_order_relaxed);
    setBootstrapStatus(bootstrap, "Startup download disabled. Use Data Library to fetch missing datasets.");

    auto layers = loadManifest(root);
    int parcel_layer_idx = -1;
    int real_property_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
    int zoning_layer_idx = -1;
    int crime_nibrs_layer_idx = -1;
    int crime_legacy_layer_idx = -1;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].file == "parcel.geojson") parcel_layer_idx = (int)i;
        else if (layers[i].file == "real_property_information.geojson") real_property_layer_idx = (int)i;
        else if (layers[i].file == "vacant_building_notices.geojson") vacant_notice_layer_idx = (int)i;
        else if (layers[i].file == "vacant_building_rehabs.geojson") vacant_rehab_layer_idx = (int)i;
        else if (layers[i].file == "tax_lien_certificate_sale_properties.geojson") tax_lien_layer_idx = (int)i;
        else if (layers[i].file == "tax_sale_list_2021.geojson") tax_sale_layer_idx = (int)i;
        else if (layers[i].file == "zoning.geojson") zoning_layer_idx = (int)i;
        else if (layers[i].file == "crime_nibrs_group_a_2022_present.geojson") crime_nibrs_layer_idx = (int)i;
        else if (layers[i].file == "crime_part_1_legacy_srs.geojson") crime_legacy_layer_idx = (int)i;
    }
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
    int vacancy_maps_generation = 0;
    int parcel_vacancy_generation_applied = -1;
    int tax_maps_generation = 0;
    int parcel_tax_generation_applied = -1;
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
    std::mutex api_layer_mutex;
    std::unordered_map<std::string, bool> api_layer_enable_cmds;
    std::unordered_map<std::string, bool> api_layer_fill_cmds;
    std::mutex layer_fill_mutex;
    std::vector<bool> layer_fill_enabled(layers.size(), true);
    std::vector<bool> layer_hover_enabled(layers.size(), true);
    std::vector<bool> layer_inspect_enabled(layers.size(), true);
    std::vector<bool> layer_heatmap_enabled(layers.size(), true);
    std::vector<int> layer_heatmap_max_zoom(layers.size(), 13);
    std::vector<bool> layer_heatmap_use_gradient(layers.size(), true);
    std::vector<int> layer_heatmap_algo(layers.size(), -1);
    std::vector<bool> layer_heatmap_use_global_settings(layers.size(), true);
    std::vector<float> layer_heatmap_cell_px(layers.size(), 24.0f);
    std::vector<float> layer_heatmap_bandwidth_px(layers.size(), 18.0f);
    std::vector<float> layer_heatmap_blur_sigma_px(layers.size(), 6.0f);
    std::vector<float> layer_heatmap_percentile_clip(layers.size(), 95.0f);
    std::vector<bool> layer_heatmap_zoom_adaptive_bandwidth(layers.size(), true);
    std::vector<bool> layer_heatmap_multires_enabled(layers.size(), true);
    std::vector<float> layer_heatmap_multires_blend(layers.size(), 0.5f);
    // Heatmap runtime settings (loaded/saved via layer_ui_state.json)
    float global_heat_cell_px = 24.0f;
    int heatmap_algo = 0; // 0=grid,1=kde,2=gpu_splat_blur,3=hex,4=multires
    int heatmap_quality_preset = 1; // 0=fast,1=balanced,2=high
    float heatmap_bandwidth_px = 18.0f;
    float heatmap_blur_sigma_px = 6.0f;
    float heatmap_percentile_clip = 95.0f;
    bool heatmap_zoom_adaptive_bandwidth = true;
    bool heatmap_multires_enabled = true;
    float heatmap_multires_blend = 0.5f;
    std::vector<CachedHeatCell> heatmap_cached_cells;
    TileTexture heatmap_raster_texture;
    bool heatmap_raster_texture_valid = false;
    HeatmapRaster heatmap_cached_raster_meta;
    uint64_t heatmap_raster_cache_key = 0;
    uint64_t heatmap_cache_key = 0;
    bool heatmap_cache_valid = false;
    std::future<std::pair<uint64_t, HeatmapRenderData>> heatmap_async_future;
    bool heatmap_async_inflight = false;
    bool hover_inspector_enabled = true;
    loadLayerUiState(
        root,
        layers,
        hover_inspector_enabled,
        &zoning_zone_enabled,
        &layer_fill_enabled,
        &layer_hover_enabled,
        &layer_inspect_enabled,
        &layer_heatmap_enabled,
        &layer_heatmap_max_zoom,
        &layer_heatmap_use_gradient,
        &layer_heatmap_algo,
        &layer_heatmap_use_global_settings,
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
        &heatmap_multires_blend);
    TimeCubeService time_cube_service(root);
    std::mutex status_mutex;
    std::vector<LayerRuntimeState> layer_states(layers.size());
    std::vector<LayerSpatialIndex> layer_spatial(layers.size());
    std::vector<uint32_t> render_candidates;
    struct OwnerAggregate {
        std::string owner;
        size_t property_count = 0;
        double area_m2 = 0.0;
        double value_usd = 0.0;
    };
    std::vector<OwnerAggregate> owner_aggregates;
    std::unordered_set<std::string> selected_owners;
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
    auto enqueue_hydration = [&](size_t idx, bool required = false) {
        if (idx >= layers.size()) return;
        std::lock_guard<std::mutex> lk(hydrate_req_mutex);
        if (required) hydration_required[idx] = true;
        if (!hydration_requested[idx]) {
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
        &render_fill_attempts_last_frame,
        &render_fill_success_last_frame,
        &render_fill_no_triangles_last_frame,
        &render_fill_bad_indices_last_frame,
        &api_layer_mutex,
        &api_layer_enable_cmds,
        &api_layer_fill_cmds,
        &api_zoom_cmd,
        &api_lon_cmd,
        &api_lat_cmd,
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
        &prof_retired_textures
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
    std::vector<bool> last_enabled_state;
    last_enabled_state.reserve(layers.size());
    for (const auto& l : layers) last_enabled_state.push_back(l.enabled);
    bool last_hover_inspector_enabled = hover_inspector_enabled;
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
    int data_library_download_phase = 0;
    bool data_library_include_large = false;
    bool data_library_bulk_inflight = false;
    std::mutex data_library_bulk_mutex;
    std::string data_library_bulk_progress;
    std::future<LayerDownloadSummary> data_library_bulk_future;
    bool filter_enabled = false;
    bool filter_use_date = false;
    int filter_year_min = 2000;
    int filter_year_max = 2026;
    char filter_blocklot[64] = "";
    char filter_status[64] = "";
    char filter_address[96] = "";
    char filter_owner[96] = "";
    char filter_zip[24] = "";
    bool crime_filter_enabled = false;
    bool crime_filter_homicide = false;
    bool crime_filter_robbery = false;
    bool crime_filter_assault = false;
    bool crime_filter_burglary = false;
    bool crime_filter_theft = false;
    bool crime_filter_auto_theft = false;
    bool crime_filter_drug = false;
    bool crime_filter_shooting = false;
    bool crime_filter_use_year = false;
    int crime_year_min = 2022;
    int crime_year_max = 2026;
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
        std::string app_version;
        int protocol_version = 0;
        int dataset_port = 0;
        bool protocol_match = false;
    };
    std::vector<LanPeerInfo> lan_peers;
    std::string lan_scan_status = "Not scanned";
    char arkavo_room_id[128] = "worldsim-default-room";
    std::string arkavo_status = "idle";
    std::string arkavo_err;
    std::unique_ptr<ArkavoRealtimeClient> arkavo_client;
    std::unique_ptr<ArkavoRtcSessionManager> arkavo_rtc;
    char arkavo_send_peer[160] = "";
    char arkavo_send_path[512] = "";

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (g_SwapChainRebuild) {
            glfwGetFramebufferSize(window, &w, &h);
            if (w > 0 && h > 0) {
                ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
                ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData, g_QueueFamily, g_Allocator, w, h, g_MinImageCount);
                g_MainWindowData.FrameIndex = 0;
                g_SwapChainRebuild = false;
            }
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
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
        auto heatmap_input_float_enter = [&](const char* label, float& value, float min_value, float max_value, const char* format) {
            static std::unordered_map<ImGuiID, float> drafts;
            static std::unordered_map<ImGuiID, bool> was_active;
            const ImGuiID id = ImGui::GetID(label);
            if (!was_active[id]) drafts[id] = value;
            float draft = drafts[id];
            const bool committed = ImGui::InputFloat(label, &draft, 0.0f, 0.0f, format, ImGuiInputTextFlags_EnterReturnsTrue);
            const bool active = ImGui::IsItemActive();
            heatmap_controls_active |= active;
            if (committed) {
                value = std::clamp(draft, min_value, max_value);
                drafts[id] = value;
                was_active[id] = active;
                return true;
            }
            drafts[id] = active ? draft : value;
            was_active[id] = active;
            return false;
        };
        auto refresh_layer_profile_snapshot = [&]() {
            bool any_dirty = false;
            for (bool dirty : layer_profile_dirty) {
                if (dirty) {
                    any_dirty = true;
                    break;
                }
            }
            if (!any_dirty) return;
            std::vector<LayerProfileSnapshot> updates;
            updates.reserve(layers.size());
            for (size_t i = 0; i < layers.size(); ++i) {
                if (i >= layer_profile_dirty.size() || !layer_profile_dirty[i]) continue;
                const auto& layer = layers[i];
                LayerProfileSnapshot s;
                s.index = i;
                s.name = layer.name;
                s.file = layer.file;
                s.enabled = layer.enabled;
                s.features = layer.features.size();
                for (const auto& fg : layer.features) {
                    s.rings += fg.rings.size();
                    s.triangle_indices += fg.triangles.size();
                    s.properties += fg.properties.size();
                    for (const auto& r : fg.rings) s.ring_points += r.size();
                }
                if (i < layer_spatial.size()) {
                    s.spatial_index_built = layer_spatial[i].built;
                    s.spatial_index_cells = layer_spatial[i].cells.size();
                    s.spatial_index_marks = layer_spatial[i].marks.size();
                }
                updates.push_back(std::move(s));
            }
            {
                std::lock_guard<std::mutex> lk(layer_profile_mutex);
                for (const auto& s : updates) {
                    if (s.index < layer_profile_snapshot.size()) layer_profile_snapshot[s.index] = s;
                }
            }
            std::fill(layer_profile_dirty.begin(), layer_profile_dirty.end(), false);
        };
        // Apply REST control commands on main thread.
        {
            int zc = api_zoom_cmd.exchange(-1, std::memory_order_relaxed);
            if (zc >= kMinZoom && zc <= kMaxZoom) zoom = zc;
            double lonc = api_lon_cmd.exchange(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
            double latc = api_lat_cmd.exchange(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
            if (!std::isnan(lonc)) center_lon = lonc;
            if (!std::isnan(latc)) center_lat = std::clamp(latc, -85.0, 85.0);
            std::lock_guard<std::mutex> lk(api_layer_mutex);
            for (const auto& kv : api_layer_enable_cmds) {
                for (size_t i = 0; i < layers.size(); ++i) {
                    if (layers[i].file == kv.first) {
                        layers[i].enabled = kv.second;
                        if (i < layer_profile_dirty.size()) layer_profile_dirty[i] = true;
                    }
                }
            }
            api_layer_enable_cmds.clear();
            if (!api_layer_fill_cmds.empty()) {
                std::lock_guard<std::mutex> lk_fill(layer_fill_mutex);
                for (const auto& kv : api_layer_fill_cmds) {
                    for (size_t i = 0; i < layers.size(); ++i) {
                        if (layers[i].file == kv.first && i < layer_fill_enabled.size()) {
                            if (layer_fill_enabled[i] != kv.second) {
                                layer_fill_enabled[i] = kv.second;
                                layer_fill_state_changed = true;
                            }
                        }
                    }
                }
                api_layer_fill_cmds.clear();
            }
        }
        current_zoom_state.store(zoom, std::memory_order_relaxed);
        current_lon_state.store(center_lon, std::memory_order_relaxed);
        current_lat_state.store(center_lat, std::memory_order_relaxed);
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(420, 760), ImGuiCond_Always);
        ImGui::Begin("Layers and Controls");
        if (ImGui::Button("Gear")) show_sources_panel = !show_sources_panel;
        ImGui::SameLine();
        if (ImGui::Button("Library")) show_data_library = !show_data_library;
        ImGui::SameLine();
        ImGui::Text("Vulkan map + Vulkan UI");
        size_t local_layer_count = 0;
        for (const auto& l : layers) {
            if (fs::exists(root / "data" / "layers" / l.file)) local_layer_count++;
        }
        ImGui::TextDisabled("Local data: %zu/%zu", local_layer_count, layers.size());
        ImGui::SliderInt("Zoom", &zoom, kMinZoom, kMaxZoom);
        const double lon_min = -76.75, lon_max = -76.45;
        const double lat_min = 39.18, lat_max = 39.40;
        ImGui::SliderScalar("Center Lon", ImGuiDataType_Double, &center_lon, &lon_min, &lon_max, "%.6f");
        ImGui::SliderScalar("Center Lat", ImGuiDataType_Double, &center_lat, &lat_min, &lat_max, "%.6f");
        ImGui::Checkbox("Enable Hover Inspector", &hover_inspector_enabled);
        ImGui::SeparatorText("Heatmap");
        const char* heatmap_algo_items[] = {
            "Grid Binning",
            "KDE (Gaussian)",
            "GPU Splat + Blur",
            "Hex Binning",
            "Multi-res Pyramid"
        };
        const char* heatmap_quality_items[] = {"Fast", "Balanced", "High"};
        int heatmap_quality_prev = heatmap_quality_preset;
        heatmap_settings_state_changed |= ImGui::Combo("Heatmap Quality", &heatmap_quality_preset, heatmap_quality_items, IM_ARRAYSIZE(heatmap_quality_items));
        heatmap_controls_active |= ImGui::IsItemActive();
        if (heatmap_quality_preset != heatmap_quality_prev) {
            if (heatmap_quality_preset == 0) {
                global_heat_cell_px = 36.0f;
                heatmap_bandwidth_px = 10.0f;
                heatmap_blur_sigma_px = 3.0f;
                heatmap_percentile_clip = 92.0f;
                heatmap_multires_enabled = false;
            } else if (heatmap_quality_preset == 1) {
                global_heat_cell_px = 24.0f;
                heatmap_bandwidth_px = 18.0f;
                heatmap_blur_sigma_px = 6.0f;
                heatmap_percentile_clip = 95.0f;
                heatmap_multires_enabled = true;
                heatmap_multires_blend = 0.5f;
            } else {
                global_heat_cell_px = 14.0f;
                heatmap_bandwidth_px = 26.0f;
                heatmap_blur_sigma_px = 10.0f;
                heatmap_percentile_clip = 97.0f;
                heatmap_multires_enabled = true;
                heatmap_multires_blend = 0.65f;
            }
            heatmap_settings_state_changed = true;
        }
        heatmap_settings_state_changed |= ImGui::Combo("Heatmap Algorithm", &heatmap_algo, heatmap_algo_items, IM_ARRAYSIZE(heatmap_algo_items));
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply Global To All")) {
            for (size_t i = 0; i < layer_heatmap_algo.size(); ++i) {
                if (i < layer_heatmap_enabled.size()) layer_heatmap_enabled[i] = true;
                layer_heatmap_algo[i] = -1; // Use global method
                if (i < layer_heatmap_use_global_settings.size()) layer_heatmap_use_global_settings[i] = true;
            }
            layer_heatmap_state_changed = true;
            heatmap_settings_state_changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Clear per-layer overrides and force all heatmap layers to use global settings.");
            ImGui::EndTooltip();
        }
        heatmap_controls_active |= ImGui::IsItemActive();
        heatmap_settings_state_changed |= heatmap_input_float_enter("Heatmap Cell (px)", global_heat_cell_px, 8.0f, 80.0f, "%.0f");
        heatmap_settings_state_changed |= heatmap_input_float_enter("Bandwidth (px)", heatmap_bandwidth_px, 2.0f, 96.0f, "%.1f");
        heatmap_settings_state_changed |= heatmap_input_float_enter("Blur Sigma (px)", heatmap_blur_sigma_px, 0.0f, 32.0f, "%.1f");
        heatmap_settings_state_changed |= heatmap_input_float_enter("Normalization Clip (%)", heatmap_percentile_clip, 50.0f, 100.0f, "%.0f");
        heatmap_settings_state_changed |= ImGui::Checkbox("Zoom-adaptive bandwidth", &heatmap_zoom_adaptive_bandwidth);
        heatmap_controls_active |= ImGui::IsItemActive();
        heatmap_settings_state_changed |= ImGui::Checkbox("Enable multi-res blending", &heatmap_multires_enabled);
        heatmap_controls_active |= ImGui::IsItemActive();
        ImGui::BeginDisabled(!heatmap_multires_enabled);
        heatmap_settings_state_changed |= heatmap_input_float_enter("Multi-res blend", heatmap_multires_blend, 0.0f, 1.0f, "%.2f");
        heatmap_controls_active |= ImGui::IsItemActive();
        ImGui::EndDisabled();
        bool validation_ui = g_EnableValidationLayers;
        if (ImGui::Checkbox("Vulkan Validation (restart required)", &validation_ui)) {
            g_EnableValidationLayers = validation_ui;
            app_settings.vulkan_validation_enabled = g_EnableValidationLayers;
            saveAppSettings(root, app_settings);
        }
        if (ImGui::Checkbox("Greyscale underlying map", &app_settings.grayscale_basemap)) {
            saveAppSettings(root, app_settings);
        }
        ImGui::Separator();

        if (ImGui::Button("Show All")) {
            for (auto& l : layers) l.enabled = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Hide All")) {
            for (auto& l : layers) l.enabled = false;
        }

        auto draw_category = [&](LayerDef::Category cat, const char* label) {
            if (!ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) return;
            std::string show_id = std::string("Show ") + label;
            std::string hide_id = std::string("Hide ") + label;
            if (ImGui::Button(show_id.c_str())) {
                for (auto& l : layers) if (l.category == cat) l.enabled = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(hide_id.c_str())) {
                for (auto& l : layers) if (l.category == cat) l.enabled = false;
            }
            std::string current_subcategory;
            for (auto& l : layers) {
                size_t idx = (size_t)(&l - &layers[0]);
                if (l.category != cat) continue;
                if (l.subcategory != current_subcategory) {
                    current_subcategory = l.subcategory;
                    if (!current_subcategory.empty()) ImGui::SeparatorText(current_subcategory.c_str());
                }
                ImGui::PushID((int)idx);
                const fs::path local_layer_path = root / "data" / "layers" / l.file;
                const bool local_layer_exists = fs::exists(local_layer_path);
                if (!local_layer_exists) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.12f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.30f, 0.14f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.46f, 0.18f, 0.08f, 1.0f));
                    bool can_download = !l.source_url.empty();
                    if (ImGui::SmallButton("D")) {
                        if (can_download) {
                            VersionedDownloadResult vd = downloadUrlVersioned(
                                l.source_url,
                                local_layer_path,
                                root / "data" / "versions");
                            if (vd.ok) {
                                data_library_status_msg = (vd.not_modified ? "Checked " : "Downloaded/updated ") + l.file + " (" + vd.message + ")";
                                data_freshness_state[idx] = FreshnessState::UpToDate;
                                data_freshness_msg[idx] = vd.message;
                                enqueue_hydration(idx, true);
                            } else {
                                data_library_status_msg = "Download failed for " + l.file + ": " + vd.message;
                                data_freshness_state[idx] = FreshnessState::Error;
                                data_freshness_msg[idx] = vd.message;
                            }
                        } else {
                            data_library_status_msg = "No source URL for " + l.file;
                        }
                    }
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted("Download missing dataset");
                        ImGui::TextDisabled("%s", can_download ? "Source available" : "No source URL in manifest");
                        ImGui::EndTooltip();
                    }
                    ImGui::SameLine();
                }
                auto icon_toggle = [&](const char* id, const char* icon, bool& value, const char* tip) -> bool {
                    ImGui::PushID(id);
                    if (value) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.78f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.55f, 0.92f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.34f, 0.62f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.16f, 0.16f, 0.22f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.32f, 0.32f, 0.55f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.42f, 0.42f, 0.42f, 0.75f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
                    }
                    bool changed = false;
                    if (ImGui::SmallButton(icon)) {
                        value = !value;
                        changed = true;
                    }
                    ImGui::PopStyleColor(4);
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(tip);
                        ImGui::TextDisabled("%s", value ? "Enabled" : "Disabled");
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                    ImGui::SameLine();
                    return changed;
                };

                icon_toggle("show", "V", l.enabled, "Show layer");
                if (ImGui::SmallButton("⚙")) ImGui::OpenPopup("layer_display_settings");
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Layer display settings");
                    ImGui::EndTooltip();
                }
                if (ImGui::BeginPopup("layer_display_settings")) {
                    ImGui::TextUnformatted(l.name.c_str());
                    ImGui::Separator();

                    bool fill_flag = (idx < layer_fill_enabled.size()) ? layer_fill_enabled[idx] : true;
                    if (ImGui::Checkbox("Fill polygons", &fill_flag) && idx < layer_fill_enabled.size()) {
                        std::lock_guard<std::mutex> lk_fill(layer_fill_mutex);
                        layer_fill_enabled[idx] = fill_flag;
                        layer_fill_state_changed = true;
                    }
                    bool hover_flag = (idx < layer_hover_enabled.size()) ? layer_hover_enabled[idx] : true;
                    if (ImGui::Checkbox("Hover inspector", &hover_flag) && idx < layer_hover_enabled.size()) {
                        layer_hover_enabled[idx] = hover_flag;
                        layer_hover_state_changed = true;
                    }
                    bool inspect_flag = (idx < layer_inspect_enabled.size()) ? layer_inspect_enabled[idx] : true;
                    if (ImGui::Checkbox("Click inspect", &inspect_flag) && idx < layer_inspect_enabled.size()) {
                        layer_inspect_enabled[idx] = inspect_flag;
                        layer_inspect_state_changed = true;
                    }

                    ImGui::SeparatorText("Heatmap");
                    const char* layer_algo_items[] = {
                        "None",
                        "Use Global",
                        "Grid Binning",
                        "KDE (Gaussian)",
                        "GPU Splat + Blur",
                        "Hex Binning",
                        "Multi-res Pyramid"
                    };
                    int layer_algo_ui = 0;
                    const bool layer_heatmap_on = idx < layer_heatmap_enabled.size() ? layer_heatmap_enabled[idx] : true;
                    if (layer_heatmap_on && idx < layer_heatmap_algo.size()) layer_algo_ui = std::clamp(layer_heatmap_algo[idx] + 2, 1, 6);
                    if (ImGui::Combo("Aggregate method", &layer_algo_ui, layer_algo_items, IM_ARRAYSIZE(layer_algo_items)) && idx < layer_heatmap_algo.size()) {
                        if (idx < layer_heatmap_enabled.size()) layer_heatmap_enabled[idx] = layer_algo_ui != 0;
                        if (layer_algo_ui != 0) layer_heatmap_algo[idx] = layer_algo_ui - 2;
                        layer_heatmap_state_changed = true;
                    }
                    const bool aggregate_none = layer_algo_ui == 0;
                    const int resolved_layer_algo = (layer_algo_ui <= 1) ? heatmap_algo : (layer_algo_ui - 2);
                    auto sync_layer_heatmap_defaults = [&](size_t layer_i) {
                        if (layer_i >= layer_heatmap_cell_px.size()) return;
                        layer_heatmap_cell_px[layer_i] = global_heat_cell_px;
                        layer_heatmap_bandwidth_px[layer_i] = heatmap_bandwidth_px;
                        layer_heatmap_blur_sigma_px[layer_i] = heatmap_blur_sigma_px;
                        layer_heatmap_percentile_clip[layer_i] = heatmap_percentile_clip;
                        layer_heatmap_zoom_adaptive_bandwidth[layer_i] = heatmap_zoom_adaptive_bandwidth;
                        layer_heatmap_multires_enabled[layer_i] = heatmap_multires_enabled;
                        layer_heatmap_multires_blend[layer_i] = heatmap_multires_blend;
                    };
                    ImGui::BeginDisabled(aggregate_none);
                    bool use_global_method_settings = idx < layer_heatmap_use_global_settings.size() ? layer_heatmap_use_global_settings[idx] : true;
                    if (ImGui::Checkbox("Use global method settings", &use_global_method_settings) && idx < layer_heatmap_use_global_settings.size()) {
                        layer_heatmap_use_global_settings[idx] = use_global_method_settings;
                        if (!use_global_method_settings) sync_layer_heatmap_defaults(idx);
                        layer_heatmap_state_changed = true;
                    }
                    if (use_global_method_settings) sync_layer_heatmap_defaults(idx);
                    ImGui::BeginDisabled(use_global_method_settings);
                    ImGui::Indent();
                    if (resolved_layer_algo == 0) {
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Grid cell size", layer_heatmap_cell_px[idx], 8.0f, 80.0f, "%.0f");
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Grid clip", layer_heatmap_percentile_clip[idx], 50.0f, 100.0f, "%.0f");
                    } else if (resolved_layer_algo == 1) {
                        layer_heatmap_state_changed |= heatmap_input_float_enter("KDE bandwidth", layer_heatmap_bandwidth_px[idx], 2.0f, 96.0f, "%.1f");
                        bool adaptive = layer_heatmap_zoom_adaptive_bandwidth[idx];
                        if (ImGui::Checkbox("Adaptive KDE bandwidth", &adaptive)) {
                            layer_heatmap_zoom_adaptive_bandwidth[idx] = adaptive;
                            layer_heatmap_state_changed = true;
                        }
                        heatmap_controls_active |= ImGui::IsItemActive();
                        layer_heatmap_state_changed |= heatmap_input_float_enter("KDE clip", layer_heatmap_percentile_clip[idx], 50.0f, 100.0f, "%.0f");
                    } else if (resolved_layer_algo == 2) {
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Splat radius", layer_heatmap_bandwidth_px[idx], 2.0f, 96.0f, "%.1f");
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Blur sigma", layer_heatmap_blur_sigma_px[idx], 0.0f, 32.0f, "%.1f");
                        bool adaptive = layer_heatmap_zoom_adaptive_bandwidth[idx];
                        if (ImGui::Checkbox("Adaptive splat radius", &adaptive)) {
                            layer_heatmap_zoom_adaptive_bandwidth[idx] = adaptive;
                            layer_heatmap_state_changed = true;
                        }
                        heatmap_controls_active |= ImGui::IsItemActive();
                    } else if (resolved_layer_algo == 3) {
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Hex size", layer_heatmap_cell_px[idx], 8.0f, 80.0f, "%.0f");
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Hex clip", layer_heatmap_percentile_clip[idx], 50.0f, 100.0f, "%.0f");
                    } else {
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Fine cell size", layer_heatmap_cell_px[idx], 8.0f, 80.0f, "%.0f");
                        bool multires_enabled = layer_heatmap_multires_enabled[idx];
                        if (ImGui::Checkbox("Enable pyramid blend", &multires_enabled)) {
                            layer_heatmap_multires_enabled[idx] = multires_enabled;
                            layer_heatmap_state_changed = true;
                        }
                        heatmap_controls_active |= ImGui::IsItemActive();
                        ImGui::BeginDisabled(!multires_enabled);
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Pyramid blend", layer_heatmap_multires_blend[idx], 0.0f, 1.0f, "%.2f");
                        ImGui::EndDisabled();
                    }
                    ImGui::Unindent();
                    ImGui::EndDisabled();
                    int hz = (idx < layer_heatmap_max_zoom.size()) ? layer_heatmap_max_zoom[idx] : 13;
                    if (ImGui::SliderInt("Heatmap max zoom", &hz, kMinZoom, kMaxZoom) && idx < layer_heatmap_max_zoom.size()) {
                        layer_heatmap_max_zoom[idx] = hz;
                        layer_heatmap_state_changed = true;
                    }
                    bool use_gradient = (idx < layer_heatmap_use_gradient.size()) ? layer_heatmap_use_gradient[idx] : true;
                    if (ImGui::Checkbox("Apply gradient colors", &use_gradient) && idx < layer_heatmap_use_gradient.size()) {
                        layer_heatmap_use_gradient[idx] = use_gradient;
                        layer_heatmap_state_changed = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::EndPopup();
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, l.color);
                ImGui::TextUnformatted(l.name.c_str());
                ImGui::PopStyleColor();
                const bool row_hovered = ImGui::IsItemHovered();
                ImGui::SameLine();
                LayerRuntimeState st;
                {
                    std::lock_guard<std::mutex> lk(status_mutex);
                    if (idx < layer_states.size()) st = layer_states[idx];
                }
                if (st.status == LayerPipelineStatus::Failed) {
                    ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "[%s]", statusToString(st.status));
                } else {
                    ImGui::TextDisabled("[%s | %zu]", statusToString(st.status), st.feature_count);
                }
                const bool status_hovered = ImGui::IsItemHovered();
                if (row_hovered || status_hovered) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(l.name.c_str());
                    ImGui::Separator();
                    ImGui::Text("Category: %s", categoryToString(l.category));
                    ImGui::Text("Status: %s", statusToString(st.status));
                    ImGui::Text("Features: %zu", st.feature_count);
                    ImGui::Text("File: %s", l.file.c_str());
                    ImGui::Text("Local: %s", local_layer_exists ? "yes" : "no");
                    if (!l.subcategory.empty()) ImGui::Text("Subcategory: %s", l.subcategory.c_str());
                    if (!l.scale.empty()) ImGui::Text("Scale: %s", l.scale.c_str());
                    if (!l.heatmap_field.empty()) ImGui::Text("Heatmap Field: %s", l.heatmap_field.c_str());
                    if (!l.description.empty()) ImGui::TextWrapped("Description: %s", l.description.c_str());
                    if (!l.source_url.empty()) ImGui::TextWrapped("Source: %s", l.source_url.c_str());
                    if (!st.error.empty()) {
                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "Error: %s", st.error.c_str());
                    }
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
            if (cat == LayerDef::Category::Safety) {
                ImGui::SeparatorText("Crime Filters");
                ImGui::Checkbox("Enable Crime Filter", &crime_filter_enabled);
                ImGui::Checkbox("Filter Crime Year", &crime_filter_use_year);
                ImGui::BeginDisabled(!crime_filter_use_year);
                ImGui::SliderInt("Crime Year Min", &crime_year_min, 1900, 2100);
                ImGui::SliderInt("Crime Year Max", &crime_year_max, 1900, 2100);
                if (crime_year_min > crime_year_max) std::swap(crime_year_min, crime_year_max);
                ImGui::EndDisabled();
                ImGui::Checkbox("Homicide", &crime_filter_homicide); ImGui::SameLine();
                ImGui::Checkbox("Robbery", &crime_filter_robbery);
                ImGui::Checkbox("Assault", &crime_filter_assault); ImGui::SameLine();
                ImGui::Checkbox("Burglary", &crime_filter_burglary);
                ImGui::Checkbox("Theft/Larceny", &crime_filter_theft); ImGui::SameLine();
                ImGui::Checkbox("Auto Theft", &crime_filter_auto_theft);
                ImGui::Checkbox("Drug/Narcotic", &crime_filter_drug); ImGui::SameLine();
                ImGui::Checkbox("Shooting", &crime_filter_shooting);
                if (ImGui::Button("Clear Crime Filters")) {
                    crime_filter_homicide = false;
                    crime_filter_robbery = false;
                    crime_filter_assault = false;
                    crime_filter_burglary = false;
                    crime_filter_theft = false;
                    crime_filter_auto_theft = false;
                    crime_filter_drug = false;
                    crime_filter_shooting = false;
                    crime_filter_use_year = false;
                    crime_filter_enabled = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("Refresh Crime Breakdown")) {
                    std::unordered_map<std::string, int> counts;
                    auto add_layer_counts = [&](int idx) {
                        if (idx < 0 || (size_t)idx >= layers.size()) return;
                        for (const auto& fg : layers[(size_t)idx].features) {
                            const std::string desc = toLowerAscii(getPropertyValue(fg, "Description"));
                            const std::string code = toLowerAscii(getPropertyValue(fg, "CrimeCode"));
                            const std::string dt = getPropertyValue(fg, "CrimeDateTime");
                            if (crime_filter_enabled) {
                                if (crime_filter_use_year) {
                                    int yr = extractYearMaybe(dt);
                                    if (yr < 0 || yr < crime_year_min || yr > crime_year_max) continue;
                                }
                            const bool any_type =
                                crime_filter_homicide || crime_filter_robbery || crime_filter_assault ||
                                crime_filter_burglary || crime_filter_theft || crime_filter_auto_theft || crime_filter_drug || crime_filter_shooting;
                                if (any_type) {
                                    auto has = [&](const char* s) { return desc.find(s) != std::string::npos || code.find(s) != std::string::npos; };
                                    bool ok = false;
                                    if (crime_filter_homicide && (has("homicide") || has("murder"))) ok = true;
                                    if (crime_filter_robbery && has("robbery")) ok = true;
                                    if (crime_filter_assault && (has("assault") || has("aggravated assault"))) ok = true;
                                    if (crime_filter_burglary && has("burglary")) ok = true;
                                    if (crime_filter_theft && (has("larceny") || has("theft"))) ok = true;
                                    if (crime_filter_auto_theft && (has("motor vehicle theft") || has("auto theft") || has("vehicle theft"))) ok = true;
                                    if (crime_filter_drug && (has("drug") || has("narcotic"))) ok = true;
                                    if (crime_filter_shooting && has("shooting")) ok = true;
                                    if (!ok) continue;
                                }
                            }
                            std::string label = trimDisplayValue(getPropertyValue(fg, "Description"));
                            if (label.empty()) label = trimDisplayValue(getPropertyValue(fg, "CrimeCode"));
                            if (label.empty()) label = "(unknown)";
                            counts[label] += 1;
                        }
                    };
                    add_layer_counts(crime_nibrs_layer_idx);
                    add_layer_counts(crime_legacy_layer_idx);
                    crime_breakdown.clear();
                    crime_breakdown.reserve(counts.size());
                    for (auto& kv : counts) crime_breakdown.push_back(kv);
                    std::sort(crime_breakdown.begin(), crime_breakdown.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                }
                ImGui::Text("Breakdown Rows: %zu", crime_breakdown.size());
            }
        };

        draw_category(LayerDef::Category::Housing, "Housing");
        draw_category(LayerDef::Category::PublicHealth, "Public Health");
        draw_category(LayerDef::Category::Safety, "Safety");
        draw_category(LayerDef::Category::Infrastructure, "Infrastructure");
        draw_category(LayerDef::Category::Zoning, "Zoning");
        bool zoning_filters_changed = false;
        if (zoning_layer_idx >= 0 && ImGui::CollapsingHeader("Zoning Filters", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button("Show All Zones")) {
                for (auto& kv : zoning_zone_enabled) {
                    if (!kv.second) zoning_filters_changed = true;
                    kv.second = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Hide All Zones")) {
                for (auto& kv : zoning_zone_enabled) {
                    if (kv.second) zoning_filters_changed = true;
                    kv.second = false;
                }
            }
            if (zoning_zone_order.empty()) {
                ImGui::TextDisabled("Zoning classes will appear after zoning layer hydrates.");
            } else {
                for (const auto& gkey : zoning_group_order) {
                    auto git = zoning_group_zones.find(gkey);
                    if (git == zoning_group_zones.end()) continue;
                    const auto& zones = git->second;
                    size_t enabled_count = 0;
                    for (const auto& z : zones) if (zoning_zone_enabled[z]) enabled_count++;
                    bool group_enabled = enabled_count == zones.size() && !zones.empty();
                    bool group_partial = enabled_count > 0 && enabled_count < zones.size();

                    if (group_partial) ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.0f, 0.75f, 0.1f, 1.0f));
                    std::string gcb = gkey + "##group_enabled";
                    bool group_next = group_enabled;
                    if (ImGui::Checkbox(gcb.c_str(), &group_next)) {
                        for (const auto& z : zones) {
                            if (zoning_zone_enabled[z] != group_next) zoning_filters_changed = true;
                            zoning_zone_enabled[z] = group_next;
                        }
                    }
                    if (group_partial) ImGui::PopStyleColor();
                    ImGui::SameLine();
                    std::string gheader = gkey + " Zones (" + std::to_string(enabled_count) + "/" + std::to_string(zones.size()) + ")";
                    if (ImGui::TreeNodeEx(gheader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                        for (const auto& zkey : zones) {
                            ImVec4 zc = zoning_zone_color[zkey];
                            ImGui::ColorButton((std::string("##zclr_") + zkey).c_str(), zc, ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
                            ImGui::SameLine();
                            bool enabled = zoning_zone_enabled[zkey];
	                            std::string display = zkey;
	                            auto lit = zoning_zone_label.find(zkey);
	                            if (lit != zoning_zone_label.end() && !lit->second.empty() && lit->second != zkey) {
	                                display += " - " + lit->second;
	                            }
	                            std::string label = display + " (" + std::to_string(zoning_zone_counts[zkey]) + ")";
	                            if (ImGui::Checkbox(label.c_str(), &enabled)) {
	                                zoning_zone_enabled[zkey] = enabled;
	                                zoning_filters_changed = true;
	                            }
	                            if (ImGui::IsItemHovered()) {
	                                ImGui::BeginTooltip();
	                                ImGui::TextUnformatted(zkey.c_str());
	                                auto mit = zoning_metadata.find(zkey);
	                                if (mit != zoning_metadata.end()) {
	                                    if (!mit->second.label.empty()) ImGui::TextWrapped("%s", mit->second.label.c_str());
	                                    if (!mit->second.description.empty()) {
	                                        ImGui::Separator();
	                                        ImGui::TextWrapped("%s", mit->second.description.c_str());
	                                    }
	                                }
	                                ImGui::EndTooltip();
	                            }
	                        }
                        ImGui::TreePop();
                    }
                }
            }
        }

        const size_t hydrated_now = hydrated_count.load(std::memory_order_relaxed);
        const size_t triangulated_now = triangulated_count.load(std::memory_order_relaxed);
        if (hydrated_now > last_hydrated_seen) {
            last_hydrated_seen = hydrated_now;
            last_hydration_progress_at = std::chrono::steady_clock::now();
        }
        if (triangulated_now > last_triangulated_seen) {
            last_triangulated_seen = triangulated_now;
            last_tri_progress_at = std::chrono::steady_clock::now();
        }

        size_t hydrated_pending = 0;
        {
            std::lock_guard<std::mutex> lk(hydrated_mutex);
            hydrated_pending = hydrated_queue.size();
        }
        size_t tri_pending = 0;
        {
            std::lock_guard<std::mutex> lk(tri_mutex);
            tri_pending = tri_jobs.size();
        }

        const float hydrated_frac = layers.empty() ? 1.0f : (float)hydrated_now / (float)layers.size();
        const float tri_frac = layers.empty() ? 1.0f : (float)triangulated_now / (float)layers.size();
        const auto now = std::chrono::steady_clock::now();
        const double elapsed_s = std::chrono::duration<double>(now - hydration_started_at).count();
        const double hydrate_idle_s = std::chrono::duration<double>(now - last_hydration_progress_at).count();
        const double tri_idle_s = std::chrono::duration<double>(now - last_tri_progress_at).count();

        if (show_sources_panel) {
            ImGui::SetNextWindowSize(ImVec2(540, 420), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Gear Panel", &show_sources_panel, ImGuiWindowFlags_NoCollapse)) {
                if (ImGui::BeginTabBar("gear_tabs")) {
                    if (ImGui::BeginTabItem("Sources")) {
                        std::vector<std::string> past_work;
                        std::vector<std::string> future_work;
                        std::vector<std::string> skipped_layer_files;
                        std::string todo_text = readTextFile(root / "TODO.md");
                        collectTodoWork(todo_text, past_work, future_work);
                        {
                            std::lock_guard<std::mutex> lk(bootstrap.msg_mutex);
                            skipped_layer_files = bootstrap.skipped_layer_files;
                        }
                        size_t skipped_layers = bootstrap.skipped_layers.load(std::memory_order_relaxed);
                        size_t skipped_tiles = bootstrap.skipped_tiles.load(std::memory_order_relaxed);
                        ImGui::Text("Past: %zu", past_work.size());
                        ImGui::SameLine();
                        ImGui::Text("Future: %zu", future_work.size());
                        ImGui::SameLine();
                        ImGui::Text("Skipped: %zuL/%zuT", skipped_layers, skipped_tiles);
                        ImGui::Separator();
                        if (ImGui::BeginTabBar("work_tabs")) {
                            if (ImGui::BeginTabItem("Past Work")) {
                                ImGui::BeginChild("past_work_scroll", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                                if (past_work.empty()) ImGui::TextDisabled("No completed checklist items found in TODO.md");
                                else for (const auto& item : past_work) ImGui::BulletText("%s", item.c_str());
                                ImGui::EndChild();
                                ImGui::EndTabItem();
                            }
                            if (ImGui::BeginTabItem("Future Work")) {
                                ImGui::BeginChild("future_work_scroll", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                                if (future_work.empty()) ImGui::TextDisabled("No pending items found in TODO.md");
                                else for (const auto& item : future_work) ImGui::BulletText("%s", item.c_str());
                                ImGui::EndChild();
                                ImGui::EndTabItem();
                            }
                            if (ImGui::BeginTabItem("Skipped This Run")) {
                                ImGui::BeginChild("skipped_work_scroll", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                                ImGui::Text("Layers already present (not downloaded this run): %zu", skipped_layers);
                                ImGui::Text("Tiles already present (not downloaded this run): %zu", skipped_tiles);
                                ImGui::Separator();
                                if (skipped_layer_files.empty()) ImGui::TextDisabled("No skipped layers recorded for this run.");
                                else {
                                    ImGui::TextUnformatted("Skipped layer files:");
                                    for (const auto& f : skipped_layer_files) ImGui::BulletText("%s", f.c_str());
                                }
                                ImGui::EndChild();
                                ImGui::EndTabItem();
                            }
                            ImGui::EndTabBar();
                        }
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Heatmap")) {
                        const char* heatmap_algo_items[] = {
                            "Grid Binning",
                            "KDE (Gaussian)",
                            "GPU Splat + Blur",
                            "Hex Binning",
                            "Multi-res Pyramid"
                        };
                        const char* heatmap_quality_items[] = {"Fast", "Balanced", "High"};
                        int heatmap_quality_prev = heatmap_quality_preset;
                        heatmap_settings_state_changed |= ImGui::Combo("Quality", &heatmap_quality_preset, heatmap_quality_items, IM_ARRAYSIZE(heatmap_quality_items));
                        heatmap_controls_active |= ImGui::IsItemActive();
                        if (heatmap_quality_preset != heatmap_quality_prev) {
                            if (heatmap_quality_preset == 0) {
                                global_heat_cell_px = 36.0f;
                                heatmap_bandwidth_px = 10.0f;
                                heatmap_blur_sigma_px = 3.0f;
                                heatmap_percentile_clip = 92.0f;
                                heatmap_multires_enabled = false;
                            } else if (heatmap_quality_preset == 1) {
                                global_heat_cell_px = 24.0f;
                                heatmap_bandwidth_px = 18.0f;
                                heatmap_blur_sigma_px = 6.0f;
                                heatmap_percentile_clip = 95.0f;
                                heatmap_multires_enabled = true;
                                heatmap_multires_blend = 0.5f;
                            } else {
                                global_heat_cell_px = 14.0f;
                                heatmap_bandwidth_px = 26.0f;
                                heatmap_blur_sigma_px = 10.0f;
                                heatmap_percentile_clip = 97.0f;
                                heatmap_multires_enabled = true;
                                heatmap_multires_blend = 0.65f;
                            }
                            heatmap_settings_state_changed = true;
                        }
                        heatmap_settings_state_changed |= ImGui::Combo("Aggregate Method", &heatmap_algo, heatmap_algo_items, IM_ARRAYSIZE(heatmap_algo_items));
                        heatmap_controls_active |= ImGui::IsItemActive();
                        heatmap_settings_state_changed |= heatmap_input_float_enter("Cell Size", global_heat_cell_px, 8.0f, 80.0f, "%.0f");
                        heatmap_settings_state_changed |= heatmap_input_float_enter("Bandwidth", heatmap_bandwidth_px, 2.0f, 96.0f, "%.1f");
                        heatmap_settings_state_changed |= heatmap_input_float_enter("Blur Sigma", heatmap_blur_sigma_px, 0.0f, 32.0f, "%.1f");
                        heatmap_settings_state_changed |= heatmap_input_float_enter("Clip", heatmap_percentile_clip, 50.0f, 100.0f, "%.0f");
                        heatmap_settings_state_changed |= ImGui::Checkbox("Zoom-adaptive bandwidth", &heatmap_zoom_adaptive_bandwidth);
                        heatmap_controls_active |= ImGui::IsItemActive();
                        heatmap_settings_state_changed |= ImGui::Checkbox("Multi-res blending", &heatmap_multires_enabled);
                        heatmap_controls_active |= ImGui::IsItemActive();
                        ImGui::BeginDisabled(!heatmap_multires_enabled);
                        heatmap_settings_state_changed |= heatmap_input_float_enter("Multi-res Blend", heatmap_multires_blend, 0.0f, 1.0f, "%.2f");
                        ImGui::EndDisabled();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            ImGui::End();
        }
        if (show_data_library) {
            ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Data Library", &show_data_library, ImGuiWindowFlags_NoCollapse)) {
                auto draw_library_cell = [](const std::string& value, size_t max_chars = 64) {
                    const std::string full = value.empty() ? std::string("-") : value;
                    std::string shown = full;
                    if (shown.size() > max_chars && max_chars > 3) shown = shown.substr(0, max_chars - 3) + "...";
                    ImGui::TextUnformatted(shown.c_str());
                    if (shown.size() != full.size() && ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 80.0f);
                        ImGui::TextUnformatted(full.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                };
                auto draw_library_tooltip = [](const LayerDef& l) {
                    if (!ImGui::IsItemHovered()) return;
                    if (l.description.empty() && l.source_url.empty()) return;
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 90.0f);
                    if (!l.description.empty()) ImGui::TextUnformatted(l.description.c_str());
                    if (!l.description.empty() && !l.source_url.empty()) ImGui::Separator();
                    if (!l.source_url.empty()) ImGui::TextUnformatted(l.source_url.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                };
                auto freshness_label = [](FreshnessState s) -> const char* {
                    switch (s) {
                        case FreshnessState::UpToDate: return "Up-to-date";
                        case FreshnessState::UpdateAvailable: return "Update available";
                        case FreshnessState::NotTrackable: return "Not trackable";
                        case FreshnessState::Error: return "Check error";
                        case FreshnessState::Unknown: default: return "Unknown";
                    }
                };
                auto freshness_color = [](FreshnessState s) -> ImVec4 {
                    switch (s) {
                        case FreshnessState::UpToDate: return ImVec4(0.20f, 0.62f, 0.25f, 1.0f);
                        case FreshnessState::UpdateAvailable: return ImVec4(0.80f, 0.50f, 0.10f, 1.0f);
                        case FreshnessState::NotTrackable: return ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
                        case FreshnessState::Error: return ImVec4(0.75f, 0.22f, 0.16f, 1.0f);
                        case FreshnessState::Unknown: default: return ImVec4(0.30f, 0.45f, 0.72f, 1.0f);
                    }
                };
                const char* download_phases[] = {"must-have", "nice-to-have", "heavy-data", "all", "capital-flows"};
                if (data_library_bulk_inflight && data_library_bulk_future.valid() &&
                    data_library_bulk_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    LayerDownloadSummary summary = data_library_bulk_future.get();
                    data_library_bulk_inflight = false;
                    data_library_status_msg = "Downloaded phase: " + std::to_string(summary.downloaded) +
                        " fetched/checked, " + std::to_string(summary.skipped) +
                        " skipped, " + std::to_string(summary.failed) + " failed";
                    {
                        std::lock_guard<std::mutex> lk(data_library_bulk_mutex);
                        data_library_bulk_progress.clear();
                    }
                    for (size_t i = 0; i < layers.size(); ++i) {
                        if (fs::exists(root / "data" / "layers" / layers[i].file)) {
                            data_freshness_state[i] = FreshnessState::UpToDate;
                            if (data_freshness_msg[i].empty() || data_freshness_msg[i] == "not downloaded") {
                                data_freshness_msg[i] = "downloaded";
                            }
                            enqueue_hydration(i, true);
                        }
                    }
                }
                ImGui::InputTextWithHint("##data_library_query", "Search by name, file, category, subcategory...", data_library_query, sizeof(data_library_query));
                ImGui::SameLine();
                if (ImGui::Button("Clear")) data_library_query[0] = '\0';
                ImGui::SameLine();
                if (ImGui::Button("Check All Updates")) {
                    size_t checked = 0;
                    size_t updates = 0;
                    for (size_t i = 0; i < layers.size(); ++i) {
                        const auto& l = layers[i];
                        const fs::path local_path = root / "data" / "layers" / l.file;
                        if (!fs::exists(local_path) || l.source_url.empty()) {
                            data_freshness_state[i] = l.source_url.empty() ? FreshnessState::NotTrackable : FreshnessState::Unknown;
                            data_freshness_msg[i] = l.source_url.empty() ? "no source URL" : "not downloaded";
                            continue;
                        }
                        FreshnessCheckResult cr = checkUrlFreshnessVersioned(l.source_url, local_path, root / "data" / "versions");
                        data_freshness_state[i] = cr.state;
                        data_freshness_msg[i] = cr.message;
                        checked++;
                        if (cr.state == FreshnessState::UpdateAvailable) updates++;
                    }
                    data_library_status_msg = "Checked " + std::to_string(checked) + " datasets; updates available: " + std::to_string(updates);
                }
                ImGui::Separator();
                ImGui::SetNextItemWidth(150.0f);
                ImGui::Combo("Download phase", &data_library_download_phase, download_phases, IM_ARRAYSIZE(download_phases));
                ImGui::SameLine();
                ImGui::Checkbox("Include large", &data_library_include_large);
                ImGui::SameLine();
                ImGui::BeginDisabled(data_library_bulk_inflight);
                if (ImGui::Button("Download Phase")) {
                    const std::string phase = download_phases[data_library_download_phase];
                    const bool include_large = data_library_include_large;
                    {
                        std::lock_guard<std::mutex> lk(data_library_bulk_mutex);
                        data_library_bulk_progress = "Starting " + phase + " download";
                    }
                    data_library_status_msg = "Downloading " + phase + " in background";
                    data_library_bulk_inflight = true;
                    data_library_bulk_future = std::async(std::launch::async, [root, phase, include_large, &data_library_bulk_mutex, &data_library_bulk_progress]() {
                        return downloadLayerManifestPhase(
                            root,
                            phase,
                            include_large,
                            [&data_library_bulk_mutex, &data_library_bulk_progress](size_t i, size_t total, const std::string& msg) {
                                std::lock_guard<std::mutex> lk(data_library_bulk_mutex);
                                data_library_bulk_progress = "[" + std::to_string(i) + "/" + std::to_string(total) + "] " + msg;
                            });
                    });
                }
                ImGui::EndDisabled();
                if (data_library_bulk_inflight) {
                    std::string progress;
                    {
                        std::lock_guard<std::mutex> lk(data_library_bulk_mutex);
                        progress = data_library_bulk_progress;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", progress.empty() ? "Downloading..." : progress.c_str());
                }
                size_t downloaded_count = 0;
                for (const auto& l : layers) {
                    if (fs::exists(root / "data" / "layers" / l.file)) downloaded_count++;
                }
                ImGui::Text("Downloaded: %zu / %zu", downloaded_count, layers.size());
                ImGui::TextDisabled("Version metadata: data/versions/metadata | snapshots: data/versions/snapshots | diffs: data/versions/diffs");
                if (!data_library_status_msg.empty()) ImGui::TextWrapped("%s", data_library_status_msg.c_str());
                ImGui::Separator();

                const std::string query = trimDisplayValue(data_library_query);
                if (query != data_library_cached_query || data_library_cached_layer_count != layers.size()) {
                    data_library_cached_query = query;
                    data_library_cached_layer_count = layers.size();
                    data_library_visible_rows.clear();
                    data_library_visible_rows.reserve(layers.size());
                    for (size_t i = 0; i < layers.size(); ++i) {
                        const auto& l = layers[i];
                        const bool hit =
                            query.empty() ||
                            containsCaseInsensitive(l.name, query) ||
                            containsCaseInsensitive(l.file, query) ||
                            containsCaseInsensitive(categoryToString(l.category), query) ||
                            containsCaseInsensitive(l.subcategory, query) ||
                            containsCaseInsensitive(l.description, query);
                        if (hit) data_library_visible_rows.push_back(i);
                    }
                    data_library_cache_rebuilds++;
                }

                data_library_rendered_rows_last = 0;
                if (ImGui::BeginTable("data_library_table", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
                    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 94.0f);
                    ImGui::TableSetupColumn("Freshness", ImGuiTableColumnFlags_WidthFixed, 142.0f);
                    ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("File");
                    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                    ImGui::TableSetupColumn("Subcategory");
                    ImGui::TableHeadersRow();
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(data_library_visible_rows.size()));
                    while (clipper.Step()) {
                        for (int display_index = clipper.DisplayStart; display_index < clipper.DisplayEnd; ++display_index) {
                            const size_t i = data_library_visible_rows[static_cast<size_t>(display_index)];
                            auto& l = layers[i];
                            const fs::path local_path = root / "data" / "layers" / l.file;
                            const bool local_exists = fs::exists(local_path);
                            data_library_rendered_rows_last++;
                            ImGui::PushID((int)i);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            if (!local_exists) {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.12f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.30f, 0.14f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.46f, 0.18f, 0.08f, 1.0f));
                                if (ImGui::SmallButton("Download")) {
                                    if (l.source_url.empty()) {
                                        data_library_status_msg = "No source URL for " + l.file;
                                    } else {
                                        VersionedDownloadResult vd = downloadUrlVersioned(
                                            l.source_url,
                                            local_path,
                                            root / "data" / "versions");
                                        if (vd.ok) {
                                            data_library_status_msg = (vd.not_modified ? "Checked " : "Downloaded/updated ") + l.file + " (" + vd.message + ")";
                                            data_freshness_state[i] = FreshnessState::UpToDate;
                                            data_freshness_msg[i] = vd.message;
                                            enqueue_hydration(i, true);
                                        } else {
                                            data_library_status_msg = "Download failed for " + l.file + ": " + vd.message;
                                            data_freshness_state[i] = FreshnessState::Error;
                                            data_freshness_msg[i] = vd.message;
                                        }
                                    }
                                }
                                ImGui::PopStyleColor(3);
                            } else if (!l.source_url.empty()) {
                                if (data_freshness_state[i] == FreshnessState::UpdateAvailable) {
                                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.74f, 0.44f, 0.12f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.53f, 0.14f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.62f, 0.36f, 0.10f, 1.0f));
                                    if (ImGui::SmallButton("Update")) {
                                        VersionedDownloadResult vd = downloadUrlVersioned(
                                            l.source_url,
                                            local_path,
                                            root / "data" / "versions");
                                        if (vd.ok) {
                                            data_freshness_state[i] = FreshnessState::UpToDate;
                                            data_freshness_msg[i] = vd.message;
                                            data_library_status_msg = "Updated " + l.file + " (" + vd.message + ")";
                                            enqueue_hydration(i, true);
                                        } else {
                                            data_freshness_state[i] = FreshnessState::Error;
                                            data_freshness_msg[i] = vd.message;
                                            data_library_status_msg = "Update failed for " + l.file + ": " + vd.message;
                                        }
                                    }
                                    ImGui::PopStyleColor(3);
                                } else if (ImGui::SmallButton("Check")) {
                                    FreshnessCheckResult cr = checkUrlFreshnessVersioned(l.source_url, local_path, root / "data" / "versions");
                                    data_freshness_state[i] = cr.state;
                                    data_freshness_msg[i] = cr.message;
                                    data_library_status_msg = "Checked " + l.file + ": " + cr.message;
                                }
                            } else {
                                ImGui::TextDisabled("-");
                            }
                            ImGui::TableSetColumnIndex(1);
                            ImGui::ColorButton("##freshness_dot", freshness_color(data_freshness_state[i]), ImGuiColorEditFlags_NoTooltip, ImVec2(10, 10));
                            ImGui::SameLine();
                            ImGui::TextDisabled("%s", freshness_label(data_freshness_state[i]));
                            if (!data_freshness_msg[i].empty() && ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                ImGui::TextUnformatted(data_freshness_msg[i].c_str());
                                ImGui::EndTooltip();
                            }
                            ImGui::TableSetColumnIndex(2);
                            ImGui::Checkbox("##enable", &l.enabled);
                            ImGui::TableSetColumnIndex(3);
                            ImGui::PushStyleColor(ImGuiCol_Text, l.color);
                            draw_library_cell(l.name, 72);
                            ImGui::PopStyleColor();
                            draw_library_tooltip(l);
                            ImGui::TableSetColumnIndex(4);
                            draw_library_cell(l.file, 72);
                            ImGui::TableSetColumnIndex(5);
                            draw_library_cell(categoryToString(l.category), 32);
                            ImGui::TableSetColumnIndex(6);
                            draw_library_cell(l.subcategory, 56);
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("Matched rows: %zu | Rendered this frame: %zu | Cache rebuilds: %zu",
                                    data_library_visible_rows.size(), data_library_rendered_rows_last, data_library_cache_rebuilds);
            }
            ImGui::End();
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(12, 784), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(420, (float)h - 796.0f), ImGuiCond_Always);
        ImGui::Begin("Performance and Stats", nullptr, ImGuiWindowFlags_NoCollapse);
        ImGui::Text("Hydration: %zu / %zu (%.1f%%)", hydrated_now, layers.size(), hydrated_frac * 100.0f);
        ImGui::ProgressBar(hydrated_frac, ImVec2(-1.0f, 0.0f));
        ImGui::Text("Triangulation: %zu / %zu (%.1f%%)", triangulated_now, layers.size(), tri_frac * 100.0f);
        ImGui::ProgressBar(tri_frac, ImVec2(-1.0f, 0.0f));
        ImGui::TextDisabled("Hydration queue: %zu | Tri queue: %zu | elapsed: %.1fs", hydrated_pending, tri_pending, elapsed_s);
        ImGui::TextDisabled("Frame: %.2f ms (last %.2f) | FPS: %.1f",
                            perf_frame_ms_avg.load(std::memory_order_relaxed),
                            perf_frame_ms_last.load(std::memory_order_relaxed),
                            perf_fps_avg.load(std::memory_order_relaxed));
        ImGui::TextDisabled("UI rows: Data Library %zu/%zu (rebuilds %zu) | People & Pay %zu/%zu (rebuilds %zu)",
                            data_library_rendered_rows_last,
                            data_library_visible_rows.size(),
                            data_library_cache_rebuilds,
                            people_pay_rendered_rows_last,
                            people_pay_visible_rows.size(),
                            people_pay_cache_rebuilds);
        ImGui::TextDisabled("Fill: %zu ok / %zu attempts | no tris %zu | bad idx %zu",
                            render_fill_success_last_frame.load(std::memory_order_relaxed),
                            render_fill_attempts_last_frame.load(std::memory_order_relaxed),
                            render_fill_no_triangles_last_frame.load(std::memory_order_relaxed),
                            render_fill_bad_indices_last_frame.load(std::memory_order_relaxed));
        ImGui::TextDisabled("API: http://127.0.0.1:8787/status");
        ImGui::TextDisabled("LAN Data API: http://0.0.0.0:8788/datasets");
        ImGui::TextDisabled("P2P signaling: /p2p/register /p2p/publish /p2p/poll");
        ImGui::SeparatorText("Arkavo Native Realtime");
        ImGui::InputText("Room ID", arkavo_room_id, sizeof(arkavo_room_id));
        if (!arkavo_client) {
            if (ImGui::Button("Connect Arkavo")) {
                ArkavoRealtimeClient::Config cfg;
                cfg.room_id = trimDisplayValue(arkavo_room_id);
                cfg.signaling_url = "wss://signaling.arkavo.org/";
                auto transport = std::make_unique<ArkavoSignalingTransportCurl>();
                arkavo_client = std::make_unique<ArkavoRealtimeClient>(cfg, std::move(transport));
                arkavo_rtc = std::make_unique<ArkavoRtcSessionManager>(*arkavo_client);
                arkavo_rtc->on_log = [&](const std::string& m) { arkavo_status = m; };
                arkavo_rtc->on_error = [&](const std::string& e) { arkavo_err = e; };
                arkavo_rtc->on_file_received = [&](const std::string& peer, const std::filesystem::path& p) {
                    arkavo_status = "received file from " + peer + ": " + p.string();
                };
                arkavo_client->on_log = [&](const std::string& m) { arkavo_status = m; };
                arkavo_client->on_error = [&](const std::string& e) { arkavo_err = e; };
                arkavo_client->on_peer_should_connect = [&](const std::string& peer_id, bool initiator) {
                    if (arkavo_rtc) arkavo_rtc->connectPeer(peer_id, initiator);
                };
                arkavo_client->on_peer_left = [&](const std::string& peer_id) {
                    if (arkavo_rtc) arkavo_rtc->removePeer(peer_id);
                };
                arkavo_client->on_signal_payload = [&](const std::string& peer_id, const nlohmann::json& payload) {
                    if (arkavo_rtc) arkavo_rtc->handleSignal(peer_id, payload);
                };
                std::string err;
                if (!arkavo_client->start(err)) {
                    arkavo_err = err;
                    arkavo_status = "connect failed";
                } else {
                    arkavo_status = "connecting";
                }
            }
        } else {
            if (ImGui::Button("Disconnect Arkavo")) {
                if (arkavo_rtc) arkavo_rtc->closeAll();
                arkavo_client->stop();
                arkavo_rtc.reset();
                arkavo_client.reset();
                arkavo_status = "disconnected";
            }
            ImGui::SameLine();
            ImGui::TextDisabled("connected=%s", arkavo_client->isConnected() ? "yes" : "no");
            ImGui::TextDisabled("self peer: %s", arkavo_client->selfPeerId().empty() ? "(none)" : arkavo_client->selfPeerId().c_str());
            ImGui::TextDisabled("tracked peers: %zu", arkavo_client->peers().size());
            if (arkavo_rtc) {
                auto open_peers = arkavo_rtc->connectedPeers();
                ImGui::TextDisabled("open data channels: %zu", open_peers.size());
                ImGui::InputText("Send Peer", arkavo_send_peer, sizeof(arkavo_send_peer));
                ImGui::InputText("Send File Path", arkavo_send_path, sizeof(arkavo_send_path));
                if (ImGui::Button("Send Arkavo File")) {
                    std::string err;
                    if (!arkavo_rtc->sendFile(trimDisplayValue(arkavo_send_peer), trimDisplayValue(arkavo_send_path), err)) {
                        arkavo_err = err;
                    } else {
                        arkavo_status = "file send queued";
                    }
                }
                if (!open_peers.empty()) {
                    ImGui::TextDisabled("open peers:");
                    for (const auto& p : open_peers) ImGui::TextDisabled("%s", p.c_str());
                }
            }
        }
        ImGui::TextDisabled("status: %s", arkavo_status.c_str());
        if (!arkavo_err.empty()) ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "arkavo: %s", arkavo_err.c_str());
        if (ImGui::Button("Scan LAN Peers")) {
            lan_peers.clear();
            if (!initNetworkSockets()) {
                lan_scan_status = "Scan failed: network init error";
            } else {
                NetSocket s = ::socket(AF_INET, SOCK_DGRAM, 0);
                if (s == kInvalidNetSocket) {
                    lan_scan_status = "Scan failed: socket error";
                } else {
                    int on = 1;
                    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
#if defined(_WIN32)
                    DWORD rcv_to = 700;
#else
                    timeval rcv_to{};
                    rcv_to.tv_sec = 0;
                    rcv_to.tv_usec = 700000;
#endif
                    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv_to), sizeof(rcv_to));
                    sockaddr_in dst{};
                    dst.sin_family = AF_INET;
                    dst.sin_port = htons(8789);
                    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
                    const char* probe = "WS3_DISCOVER_V1";
                    (void)netSendTo(s, probe, std::strlen(probe), (sockaddr*)&dst, sizeof(dst));
                    std::unordered_set<std::string> seen;
                    for (;;) {
                        sockaddr_in src{};
                        NetSockLen slen = sizeof(src);
                        char rbuf[4096];
                        NetSSize rn = netRecvFrom(s, rbuf, sizeof(rbuf) - 1, (sockaddr*)&src, &slen);
                        if (rn <= 0) break;
                        rbuf[rn] = '\0';
                        json jr = json::parse(std::string(rbuf), nullptr, false);
                        if (jr.is_discarded()) continue;
                        char ipbuf[INET_ADDRSTRLEN] = {0};
                        inet_ntop(AF_INET, &src.sin_addr, ipbuf, sizeof(ipbuf));
                        const std::string ip(ipbuf);
                        if (seen.count(ip)) continue;
                        seen.insert(ip);
                        LanPeerInfo p;
                        p.ip = ip;
                        p.app_version = jr.value("app_version", "");
                        p.protocol_version = jr.value("protocol_version", 0);
                        p.dataset_port = jr.value("dataset_port", 8788);
                        p.protocol_match = (p.protocol_version == kProtocolVersion);
                        lan_peers.push_back(std::move(p));
                    }
                    netClose(s);
                    size_t compatible = 0;
                    for (const auto& p : lan_peers) if (p.protocol_match) compatible++;
                    lan_scan_status = "Peers: " + std::to_string(lan_peers.size()) +
                                      " | Compatible protocol: " + std::to_string(compatible);
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", lan_scan_status.c_str());
        if (!lan_peers.empty()) {
            ImGui::BeginChild("lan_peer_list", ImVec2(0, 90), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            for (const auto& p : lan_peers) {
                ImGui::Text("%s:%d | app %s | protocol %d %s",
                    p.ip.c_str(),
                    p.dataset_port,
                    p.app_version.empty() ? "(unknown)" : p.app_version.c_str(),
                    p.protocol_version,
                    p.protocol_match ? "[OK]" : "[MISMATCH]");
            }
            ImGui::EndChild();
        }
        if (hydrated_now < layers.size() && hydrate_idle_s > 15.0) {
            ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "Hydration has not advanced for %.1fs", hydrate_idle_s);
        }
        if (triangulated_now < layers.size() && tri_idle_s > 15.0) {
            ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "Triangulation has not advanced for %.1fs", tri_idle_s);
        }
        ImGui::Separator();
        ImGui::TextDisabled("Tile cache: %zu / %zu", g_TileCache.size(), kMaxTileCache);
        if (ImGui::Button("Clear Cache")) {
            size_t removed_files = 0;
            std::error_code ec;
            auto clear_tree = [&](const fs::path& p) {
                if (fs::exists(p, ec)) removed_files += fs::remove_all(p, ec);
            };
            clear_tree(root / "data" / "cache" / "hydration");
            clear_tree(root / "data" / "cache" / "triangulation");
            clear_tree(root / "data" / "cache" / "derived");
            fs::create_directories(root / "data" / "cache" / "hydration", ec);
            fs::create_directories(root / "data" / "cache" / "triangulation", ec);
            fs::create_directories(root / "data" / "cache" / "derived", ec);

            for (auto& kv : g_TileCache) destroyTileTexture(kv.second.tex);
            g_TileCache.clear();
            g_TileLRU.clear();

            {
                std::lock_guard<std::mutex> lk(hydrated_mutex);
                hydrated_queue.clear();
            }
            {
                std::lock_guard<std::mutex> lk(tri_mutex);
                tri_jobs.clear();
                tri_results.clear();
            }
            {
                std::lock_guard<std::mutex> lk(hydrate_req_mutex);
                hydrate_requests.clear();
                std::fill(hydration_requested.begin(), hydration_requested.end(), false);
                std::fill(hydration_required.begin(), hydration_required.end(), false);
            }

            hydrated_count.store(0, std::memory_order_relaxed);
            triangulated_count.store(0, std::memory_order_relaxed);
            cached_real_property_size = 0;
            cached_vac_notice_size = 0;
            cached_vac_rehab_size = 0;
            cached_tax_lien_size = 0;
            cached_tax_sale_size = 0;
            vacancy_maps_generation = 0;
            parcel_vacancy_generation_applied = -1;
            tax_maps_generation = 0;
            parcel_tax_generation_applied = -1;
            parcel_vac_notice_by_feature.clear();
            parcel_vac_rehab_by_feature.clear();
            parcel_tax_lien_by_feature.clear();
            parcel_tax_sale_by_feature.clear();
            parcel_tax_lien_amount_by_feature.clear();
            parcel_tax_sale_amount_by_feature.clear();
            vacant_notice_count_by_blocklot.clear();
            vacant_rehab_count_by_blocklot.clear();
            tax_lien_count_by_blocklot.clear();
            tax_lien_amount_by_blocklot.clear();
            tax_sale_count_by_blocklot.clear();
            tax_sale_amount_by_blocklot.clear();
            real_property_by_blocklot.clear();
            zoning_zone_label.clear();
            zoning_zone_counts.clear();
            zoning_zone_order.clear();
            zoning_group_zones.clear();
            zoning_group_order.clear();
            zoning_zone_discovered_feature_count = 0;
            visible_vacant_parcels_last_frame.store(0, std::memory_order_relaxed);
            vacant_parcels_matched_total.store(0, std::memory_order_relaxed);
            vacant_parcels_with_geometry_total.store(0, std::memory_order_relaxed);
            vacant_parcels_triangulated_renderable_total.store(0, std::memory_order_relaxed);

            for (size_t i = 0; i < layers.size(); ++i) {
                layers[i].features.clear();
                layer_spatial[i] = LayerSpatialIndex{};
                if (i < layer_profile_dirty.size()) layer_profile_dirty[i] = true;
                std::lock_guard<std::mutex> lk(status_mutex);
                if (i < layer_states.size()) {
                    layer_states[i].status = LayerPipelineStatus::Queued;
                    layer_states[i].feature_count = 0;
                    layer_states[i].error.clear();
                }
            }

            for (size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].enabled) enqueue_hydration(i);
            }
            last_cache_clear_msg = "Cache cleared; rehydrating enabled layers.";
        }
        if (!last_cache_clear_msg.empty()) ImGui::TextDisabled("%s", last_cache_clear_msg.c_str());
        ImGui::End();
        bool ui_state_changed =
            (hover_inspector_enabled != last_hover_inspector_enabled) ||
            zoning_filters_changed ||
            layer_fill_state_changed ||
            layer_hover_state_changed ||
            layer_inspect_state_changed ||
            layer_heatmap_state_changed ||
            heatmap_settings_state_changed;
        std::vector<size_t> newly_enabled;
        if (last_enabled_state.size() == layers.size()) {
            for (size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].enabled != last_enabled_state[i]) {
                    ui_state_changed = true;
                    if (i < layer_profile_dirty.size()) layer_profile_dirty[i] = true;
                    if (layers[i].enabled && !last_enabled_state[i]) newly_enabled.push_back(i);
                }
            }
        } else {
            ui_state_changed = true;
            for (size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].enabled) newly_enabled.push_back(i);
            }
        }
        if (!newly_enabled.empty()) {
            for (size_t i : newly_enabled) enqueue_hydration(i);
        }
        // Vacant overlays depend on parcel geometry even when parcel visibility is off.
        const bool vacant_layer_active =
            (vacant_notice_layer_idx >= 0 && layers[(size_t)vacant_notice_layer_idx].enabled) ||
            (vacant_rehab_layer_idx >= 0 && layers[(size_t)vacant_rehab_layer_idx].enabled);
        if (vacant_layer_active && parcel_layer_idx >= 0) {
            bool parcel_ready = false;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if ((size_t)parcel_layer_idx < layer_states.size()) {
                    LayerPipelineStatus st = layer_states[(size_t)parcel_layer_idx].status;
                    parcel_ready = (st == LayerPipelineStatus::Hydrated ||
                                    st == LayerPipelineStatus::TriQueued ||
                                    st == LayerPipelineStatus::Triangulating ||
                                    st == LayerPipelineStatus::Ready);
                }
            }
            if (!parcel_ready) {
                enqueue_hydration((size_t)parcel_layer_idx, true);
            }
        }
        if (real_property_layer_idx >= 0 && (size_t)real_property_layer_idx < layers.size()) {
            bool real_property_ready = false;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if ((size_t)real_property_layer_idx < layer_states.size()) {
                    LayerPipelineStatus st = layer_states[(size_t)real_property_layer_idx].status;
                    real_property_ready = (st == LayerPipelineStatus::Hydrated ||
                                           st == LayerPipelineStatus::TriQueued ||
                                           st == LayerPipelineStatus::Triangulating ||
                                           st == LayerPipelineStatus::Ready);
                }
            }
            const bool filter_join_needed =
                filter_enabled ||
                (parcel_layer_idx >= 0 && layers[(size_t)parcel_layer_idx].enabled) ||
                vacant_layer_active ||
                filter_owner[0] != '\0' ||
                filter_address[0] != '\0' ||
                filter_zip[0] != '\0';
            if (filter_join_needed && !real_property_ready) {
                enqueue_hydration((size_t)real_property_layer_idx, true);
            }
        }
        const bool tax_layer_active =
            (tax_lien_layer_idx >= 0 && layers[(size_t)tax_lien_layer_idx].enabled) ||
            (tax_sale_layer_idx >= 0 && layers[(size_t)tax_sale_layer_idx].enabled);
        if (tax_layer_active && parcel_layer_idx >= 0) {
            bool parcel_ready = false;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if ((size_t)parcel_layer_idx < layer_states.size()) {
                    LayerPipelineStatus st = layer_states[(size_t)parcel_layer_idx].status;
                    parcel_ready = (st == LayerPipelineStatus::Hydrated ||
                                    st == LayerPipelineStatus::TriQueued ||
                                    st == LayerPipelineStatus::Triangulating ||
                                    st == LayerPipelineStatus::Ready);
                }
            }
            if (!parcel_ready) enqueue_hydration((size_t)parcel_layer_idx, true);
        }
        if (zoning_layer_idx >= 0 && (size_t)zoning_layer_idx < layers.size()) {
            bool zoning_ready = false;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if ((size_t)zoning_layer_idx < layer_states.size()) {
                    LayerPipelineStatus st = layer_states[(size_t)zoning_layer_idx].status;
                    zoning_ready = (st == LayerPipelineStatus::Hydrated ||
                                    st == LayerPipelineStatus::TriQueued ||
                                    st == LayerPipelineStatus::Triangulating ||
                                    st == LayerPipelineStatus::Ready);
                }
            }
            if (layers[(size_t)zoning_layer_idx].enabled && !zoning_ready) {
                enqueue_hydration((size_t)zoning_layer_idx);
            }
        }
        if (ui_state_changed) {
            saveLayerUiState(
                root,
                layers,
                hover_inspector_enabled,
                &zoning_zone_enabled,
                &layer_fill_enabled,
                &layer_hover_enabled,
                &layer_inspect_enabled,
                &layer_heatmap_enabled,
                &layer_heatmap_max_zoom,
                &layer_heatmap_use_gradient,
                &layer_heatmap_algo,
                &layer_heatmap_use_global_settings,
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
                &heatmap_multires_blend);
            last_enabled_state.clear();
            last_enabled_state.reserve(layers.size());
            for (const auto& l : layers) last_enabled_state.push_back(l.enabled);
            last_hover_inspector_enabled = hover_inspector_enabled;
        }
        {
            std::lock_guard<std::mutex> lk(hydrated_mutex);
            while (!hydrated_queue.empty()) {
                HydratedLayer ready = std::move(hydrated_queue.front());
                hydrated_queue.pop_front();
                if (ready.index < layers.size()) {
                    if (!ready.features.empty()) {
                        auto& dst = layers[ready.index].features;
                        dst.insert(
                            dst.end(),
                            std::make_move_iterator(ready.features.begin()),
                            std::make_move_iterator(ready.features.end()));
                        if (ready.index < layer_profile_dirty.size()) layer_profile_dirty[ready.index] = true;
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (ready.index < layer_states.size()) {
                            layer_states[ready.index].status = LayerPipelineStatus::Hydrating;
                            layer_states[ready.index].feature_count = layers[ready.index].features.size();
                        }
                    }
                    if (ready.failed) {
                        {
                            std::lock_guard<std::mutex> lk4(hydrate_req_mutex);
                            if (ready.index < hydration_requested.size()) hydration_requested[ready.index] = false;
                        }
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (ready.index < layer_states.size()) {
                            layer_states[ready.index].status = LayerPipelineStatus::Failed;
                            layer_states[ready.index].error = ready.error;
                        }
                        continue;
                    }
                    if (!ready.done) continue;
                    {
                        std::lock_guard<std::mutex> lk4(hydrate_req_mutex);
                        if (ready.index < hydration_requested.size()) hydration_requested[ready.index] = false;
                    }
                    hydrated_count.fetch_add(1, std::memory_order_relaxed);
                    {
                        fs::path layer_path = root / "data" / "layers" / layers[ready.index].file;
                        fs::path cache_path = root / "data" / "cache" / "hydration" / (layers[ready.index].file + ".msgpack");
                        saveHydrationCache(cache_path, fileSignature(layer_path), layers[ready.index].features);
                    }
                    {
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (ready.index < layer_states.size()) {
                            layer_states[ready.index].status = LayerPipelineStatus::Hydrated;
                            layer_states[ready.index].feature_count = layers[ready.index].features.size();
                            layer_states[ready.index].error.clear();
                        }
                    }
                    TriJob tj;
                    tj.index = ready.index;
                    tj.file = layers[ready.index].file;
                    tj.rings_per_feature.reserve(layers[ready.index].features.size());
                    for (const auto& fg : layers[ready.index].features) tj.rings_per_feature.push_back(fg.rings);
                    std::lock_guard<std::mutex> lk2(tri_mutex);
                    {
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (ready.index < layer_states.size()) layer_states[ready.index].status = LayerPipelineStatus::TriQueued;
                    }
                    const bool parcel_dep_priority =
                        vacant_layer_active && parcel_layer_idx >= 0 && (int)ready.index == parcel_layer_idx;
                    if (layers[ready.index].enabled || parcel_dep_priority) {
                        tri_jobs.push_front(std::move(tj));
                    } else {
                        tri_jobs.push_back(std::move(tj));
                    }
                    tri_cv.notify_one();
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(tri_mutex);
            while (!tri_results.empty()) {
                TriResult tr = std::move(tri_results.front());
                tri_results.pop_front();
                if (tr.index < layers.size()) {
                    if (tr.ok) {
                        auto& fs = layers[tr.index].features;
                        size_t n = std::min(fs.size(), tr.triangles_per_feature.size());
                        for (size_t i = 0; i < n; ++i) fs[i].triangles = std::move(tr.triangles_per_feature[i]);
                        if (tr.index < layer_profile_dirty.size()) layer_profile_dirty[tr.index] = true;
                        triangulated_count.fetch_add(1, std::memory_order_relaxed);
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (tr.index < layer_states.size()) layer_states[tr.index].status = LayerPipelineStatus::Ready;
                    } else {
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (tr.index < layer_states.size()) {
                            layer_states[tr.index].status = LayerPipelineStatus::Failed;
                            layer_states[tr.index].error = tr.error;
                        }
                    }
                }
            }
        }

        if (zoning_layer_idx >= 0 && (size_t)zoning_layer_idx < layers.size()) {
            const auto& zfeats = layers[(size_t)zoning_layer_idx].features;
            if (zfeats.size() != zoning_zone_discovered_feature_count) {
                zoning_zone_discovered_feature_count = zfeats.size();
                zoning_zone_counts.clear();
                zoning_zone_label.clear();
                zoning_group_zones.clear();
                zoning_group_order.clear();
                std::unordered_map<std::string, bool> prev_enabled = zoning_zone_enabled;
                zoning_zone_order.clear();
                std::unordered_set<std::string> seen_zone_keys;
                seen_zone_keys.reserve(zfeats.size() / 4 + 16);
                for (const auto& fg : zfeats) {
                    std::string zkey = zoningClassKey(fg);
                    std::string zlabel = zoningClassLabel(fg);
                    zoning_zone_counts[zkey] += 1;
                    if (seen_zone_keys.insert(zkey).second) zoning_zone_order.push_back(zkey);
                    if (zoning_zone_enabled.find(zkey) == zoning_zone_enabled.end()) {
                        auto it_prev = prev_enabled.find(zkey);
                        zoning_zone_enabled[zkey] = (it_prev == prev_enabled.end()) ? true : it_prev->second;
                    }
                    auto meta_it = zoning_metadata.find(zkey);
                    if (zoning_zone_label.find(zkey) == zoning_zone_label.end()) {
                        zoning_zone_label[zkey] =
                            (meta_it != zoning_metadata.end() && !meta_it->second.label.empty()) ? meta_it->second.label : zlabel;
                    }
                    if (zoning_zone_color.find(zkey) == zoning_zone_color.end()) {
                        zoning_zone_color[zkey] =
                            (meta_it != zoning_metadata.end() && meta_it->second.has_color) ? meta_it->second.color : colorFromStableKey(zkey);
                    }
                }
                std::sort(zoning_zone_order.begin(), zoning_zone_order.end());
                for (const auto& zkey : zoning_zone_order) {
                    std::string g = zoningGroupKey(zkey);
                    if (zoning_group_zones.find(g) == zoning_group_zones.end()) zoning_group_order.push_back(g);
                    zoning_group_zones[g].push_back(zkey);
                }
                std::sort(zoning_group_order.begin(), zoning_group_order.end());
            }
        }

        if (real_property_layer_idx >= 0) {
            const auto& feats = layers[(size_t)real_property_layer_idx].features;
            if (feats.size() != cached_real_property_size) {
                real_property_by_blocklot.clear();
                for (size_t i = 0; i < feats.size(); ++i) {
                    std::string bl = featureBlockLotJoinKey(feats[i]);
                    if (!bl.empty() && real_property_by_blocklot.find(bl) == real_property_by_blocklot.end()) {
                        real_property_by_blocklot[bl] = i;
                    }
                }
                cached_real_property_size = feats.size();
                owner_aggregates_dirty = true;
            }
        }
        if (vacant_notice_layer_idx >= 0) {
            const auto& feats = layers[(size_t)vacant_notice_layer_idx].features;
            if (feats.size() != cached_vac_notice_size) {
                vacant_notice_count_by_blocklot.clear();
                for (const auto& fg : feats) {
                    std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
                    if (!bl.empty()) vacant_notice_count_by_blocklot[bl] += 1;
                }
                cached_vac_notice_size = feats.size();
                vacancy_maps_generation += 1;
            }
        }
        if (vacant_rehab_layer_idx >= 0) {
            const auto& feats = layers[(size_t)vacant_rehab_layer_idx].features;
            if (feats.size() != cached_vac_rehab_size) {
                vacant_rehab_count_by_blocklot.clear();
                for (const auto& fg : feats) {
                    std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
                    if (!bl.empty()) vacant_rehab_count_by_blocklot[bl] += 1;
                }
                cached_vac_rehab_size = feats.size();
                vacancy_maps_generation += 1;
            }
        }
        if (tax_lien_layer_idx >= 0) {
            const auto& feats = layers[(size_t)tax_lien_layer_idx].features;
            if (feats.size() != cached_tax_lien_size) {
                tax_lien_count_by_blocklot.clear();
                tax_lien_amount_by_blocklot.clear();
                for (const auto& fg : feats) {
                    std::string bl = featureBlockLotJoinKey(fg);
                    if (bl.empty()) continue;
                    tax_lien_count_by_blocklot[bl] += 1;
                    tax_lien_amount_by_blocklot[bl] += parseNumericField(getPropertyValue(fg, "TOTAL_AMOUNT"));
                }
                cached_tax_lien_size = feats.size();
                tax_maps_generation += 1;
            }
        }
        if (tax_sale_layer_idx >= 0) {
            const auto& feats = layers[(size_t)tax_sale_layer_idx].features;
            if (feats.size() != cached_tax_sale_size) {
                tax_sale_count_by_blocklot.clear();
                tax_sale_amount_by_blocklot.clear();
                for (const auto& fg : feats) {
                    std::string bl = featureBlockLotJoinKey(fg);
                    if (bl.empty()) continue;
                    tax_sale_count_by_blocklot[bl] += 1;
                    double amount = parseNumericField(getPropertyValue(fg, "total_lien"));
                    if (amount <= 0.0) amount = parseNumericField(getPropertyValue(fg, "total_3yea"));
                    if (amount <= 0.0) amount = parseNumericField(getPropertyValue(fg, "total_tax"));
                    tax_sale_amount_by_blocklot[bl] += amount;
                }
                cached_tax_sale_size = feats.size();
                tax_maps_generation += 1;
            }
        }
        if (parcel_layer_idx >= 0) {
            const auto& pfeats = layers[(size_t)parcel_layer_idx].features;
            if (parcel_vac_notice_by_feature.size() != pfeats.size() ||
                parcel_vac_rehab_by_feature.size() != pfeats.size() ||
                parcel_vacancy_generation_applied != vacancy_maps_generation) {
                parcel_vac_notice_by_feature.assign(pfeats.size(), 0);
                parcel_vac_rehab_by_feature.assign(pfeats.size(), 0);
                size_t notice_rows_matched = 0;
                size_t rehab_rows_matched = 0;
                for (size_t i = 0; i < pfeats.size(); ++i) {
                    std::string bl = normalizeJoinKey(getPropertyValue(pfeats[i], "BLOCKLOT"));
                    auto itn = vacant_notice_count_by_blocklot.find(bl);
                    if (itn != vacant_notice_count_by_blocklot.end()) {
                        parcel_vac_notice_by_feature[i] = itn->second;
                        notice_rows_matched += (size_t)itn->second;
                    }
                    auto itr = vacant_rehab_count_by_blocklot.find(bl);
                    if (itr != vacant_rehab_count_by_blocklot.end()) {
                        parcel_vac_rehab_by_feature[i] = itr->second;
                        rehab_rows_matched += (size_t)itr->second;
                    }
                }
                parcel_vacancy_generation_applied = vacancy_maps_generation;
                vacant_notice_rows_matched_total.store(notice_rows_matched, std::memory_order_relaxed);
                vacant_rehab_rows_matched_total.store(rehab_rows_matched, std::memory_order_relaxed);
                const fs::path derived_path = root / "data" / "cache" / "derived" / "parcel_vacancy_status.json";
                saveDerivedVacancyStatus(
                    derived_path,
                    pfeats,
                    parcel_vac_notice_by_feature,
                    parcel_vac_rehab_by_feature,
                    (size_t)cached_vac_notice_size,
                    (size_t)cached_vac_rehab_size,
                    notice_rows_matched,
                    rehab_rows_matched);
                owner_aggregates_dirty = true;
            }
            if (parcel_tax_lien_by_feature.size() != pfeats.size() ||
                parcel_tax_sale_by_feature.size() != pfeats.size() ||
                parcel_tax_generation_applied != tax_maps_generation) {
                parcel_tax_lien_by_feature.assign(pfeats.size(), 0);
                parcel_tax_sale_by_feature.assign(pfeats.size(), 0);
                parcel_tax_lien_amount_by_feature.assign(pfeats.size(), 0.0);
                parcel_tax_sale_amount_by_feature.assign(pfeats.size(), 0.0);
                for (size_t i = 0; i < pfeats.size(); ++i) {
                    std::string bl = featureBlockLotJoinKey(pfeats[i]);
                    auto it_lien = tax_lien_count_by_blocklot.find(bl);
                    if (it_lien != tax_lien_count_by_blocklot.end()) {
                        parcel_tax_lien_by_feature[i] = it_lien->second;
                        auto it_amt = tax_lien_amount_by_blocklot.find(bl);
                        if (it_amt != tax_lien_amount_by_blocklot.end()) parcel_tax_lien_amount_by_feature[i] = it_amt->second;
                    }
                    auto it_sale = tax_sale_count_by_blocklot.find(bl);
                    if (it_sale != tax_sale_count_by_blocklot.end()) {
                        parcel_tax_sale_by_feature[i] = it_sale->second;
                        auto it_amt = tax_sale_amount_by_blocklot.find(bl);
                        if (it_amt != tax_sale_amount_by_blocklot.end()) parcel_tax_sale_amount_by_feature[i] = it_amt->second;
                    }
                }
                parcel_tax_generation_applied = tax_maps_generation;
                owner_aggregates_dirty = true;
            }
            size_t matched_total = 0;
            size_t with_geometry_total = 0;
            size_t triangulated_renderable_total = 0;
            for (size_t i = 0; i < pfeats.size(); ++i) {
                const int vac_notice = (i < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[i] : 0;
                const int vac_rehab = (i < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[i] : 0;
                if ((vac_notice + vac_rehab) <= 0) continue;
                matched_total++;
                if (!pfeats[i].rings.empty()) with_geometry_total++;
                if (!pfeats[i].rings.empty() && !pfeats[i].triangles.empty()) triangulated_renderable_total++;
            }
            vacant_parcels_matched_total.store(matched_total, std::memory_order_relaxed);
            vacant_parcels_with_geometry_total.store(with_geometry_total, std::memory_order_relaxed);
            vacant_parcels_triangulated_renderable_total.store(triangulated_renderable_total, std::memory_order_relaxed);
        }
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
        refresh_layer_profile_snapshot();

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
        auto text_prop = [](const char* label, const std::string& value) {
            if (!value.empty()) ImGui::TextWrapped("%s: %s", label, value.c_str());
        };
        auto money_prop = [](const char* label, const std::string& value) {
            if (value.empty()) return;
            const double amount = parseNumericField(value);
            if (amount > 0.0) ImGui::TextWrapped("%s: $%.2f", label, amount);
            else ImGui::TextWrapped("%s: %s", label, value.c_str());
        };
        auto draw_real_property_summary = [&](const LayerDef::FeatureGeom* rp) {
            ImGui::Separator();
            ImGui::TextUnformatted("Ownership / Assessment");
            if (!rp) {
                ImGui::TextDisabled("No matching Real Property Information record loaded for this parcel.");
                ImGui::TextDisabled("Source expected: data/layers/real_property_information.geojson");
                return;
            }
            text_prop("Owner 1", firstDisplayProperty(*rp, {"OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME"}));
            text_prop("Owner 2", firstDisplayProperty(*rp, {"OWNER_2"}));
            text_prop("Owner 3", firstDisplayProperty(*rp, {"OWNER_3"}));
            text_prop("Owner Abbrev", firstDisplayProperty(*rp, {"OWNER_ABBR"}));
            text_prop("Mailing Address", firstDisplayProperty(*rp, {"MAILTOADD"}));
            text_prop("Property Address", firstDisplayProperty(*rp, {"FULLADDR", "PROPERTY_ADDRESS", "PREMISEADD", "ADDRESS"}));
            text_prop("Use Group", firstDisplayProperty(*rp, {"USEGROUP"}));
            text_prop("DHCD Use", firstDisplayProperty(*rp, {"DHCDUSE1"}));
            text_prop("Zone Code", firstDisplayProperty(*rp, {"ZONECODE"}));
            text_prop("Year Built", firstDisplayProperty(*rp, {"YEAR_BUILD"}));
            text_prop("Lot Size", firstDisplayProperty(*rp, {"LOT_SIZE"}));
            text_prop("Structure Area", firstDisplayProperty(*rp, {"STRUCTAREA"}));
            money_prop("Current Land", firstDisplayProperty(*rp, {"CURRLAND"}));
            money_prop("Current Improvements", firstDisplayProperty(*rp, {"CURRIMPR"}));
            money_prop("Tax Base", firstDisplayProperty(*rp, {"TAXBASE", "ARTAXBAS"}));
            money_prop("City Tax", firstDisplayProperty(*rp, {"CITY_TAX"}));
            money_prop("State Tax", firstDisplayProperty(*rp, {"STATETAX"}));
            money_prop("Sale Price", firstDisplayProperty(*rp, {"SALEPRIC"}));
            text_prop("Sale Date", firstDisplayProperty(*rp, {"SALEDATE"}));
            const std::string deed_book = firstDisplayProperty(*rp, {"DEEDBOOK"});
            const std::string deed_page = firstDisplayProperty(*rp, {"DEEDPAGE"});
            text_prop("Deed", deed_book.empty() ? "" : deed_book + (deed_page.empty() ? "" : " / " + deed_page));
            text_prop("SDAT Link", firstDisplayProperty(*rp, {"SDATLINK"}));
            ImGui::TextDisabled("Source: Baltimore Real Property Information (data/layers/real_property_information.geojson)");
        };
        auto draw_feature_properties = [&](const char* title, const LayerDef::FeatureGeom& fg) {
            ImGui::TextUnformatted(title);
            for (const auto& kv : fg.properties) {
                std::string v = trimDisplayValue(kv.second);
                if (v.empty()) continue;
                ImGui::TextWrapped("%s: %s", kv.first.c_str(), v.c_str());
            }
        };
        auto parcel_area_sq_m = [&](const LayerDef::FeatureGeom& fg) -> double {
            if (fg.rings.empty()) return 0.0;
            const double deg_to_m_lat = 111320.0;
            double total = 0.0;
            for (const auto& ring : fg.rings) {
                if (ring.size() < 3) continue;
                double lat_sum = 0.0;
                for (const auto& p : ring) lat_sum += (double)p.y;
                const double lat0 = lat_sum / (double)ring.size();
                const double cos_lat = std::cos(lat0 * std::numbers::pi / 180.0);
                const double sx = deg_to_m_lat * cos_lat;
                const double sy = deg_to_m_lat;
                double a = 0.0;
                for (size_t i = 0, n = ring.size(); i < n; ++i) {
                    const auto& p = ring[i];
                    const auto& q = ring[(i + 1) % n];
                    const double px = (double)p.x * sx;
                    const double py = (double)p.y * sy;
                    const double qx = (double)q.x * sx;
                    const double qy = (double)q.y * sy;
                    a += (px * qy - qx * py);
                }
                total += std::abs(a) * 0.5;
            }
            return total;
        };
        auto owner_name_for = [&](const LayerDef::FeatureGeom* rp) -> std::string {
            if (!rp) return "";
            std::string o = firstDisplayProperty(*rp, {"OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR"});
            return toLowerAscii(trimDisplayValue(o));
        };
        auto owner_value_for = [&](const LayerDef::FeatureGeom* rp) -> double {
            if (!rp) return 0.0;
            auto v = [&](std::initializer_list<const char*> keys) -> double {
                return parseNumericField(firstDisplayProperty(*rp, keys));
            };
            double tax_base = v({"TAXBASE", "ARTAXBAS"});
            double curr_land = v({"CURRLAND"});
            double curr_impr = v({"CURRIMPR"});
            double sale = v({"SALEPRIC"});
            if (tax_base > 0.0) return tax_base;
            if (curr_land + curr_impr > 0.0) return curr_land + curr_impr;
            return std::max(0.0, sale);
        };
        if (parcel_layer_idx >= 0 && (size_t)parcel_layer_idx < layers.size() &&
            layers[(size_t)parcel_layer_idx].features.size() != owner_cached_parcel_size) {
            owner_cached_parcel_size = layers[(size_t)parcel_layer_idx].features.size();
            owner_aggregates_dirty = true;
        }
        if (real_property_layer_idx >= 0 && (size_t)real_property_layer_idx < layers.size() &&
            layers[(size_t)real_property_layer_idx].features.size() != owner_cached_real_property_size) {
            owner_cached_real_property_size = layers[(size_t)real_property_layer_idx].features.size();
            owner_aggregates_dirty = true;
        }
        if (owner_aggregates_dirty && parcel_layer_idx >= 0 && (size_t)parcel_layer_idx < layers.size()) {
            const auto owner_prof_begin = std::chrono::steady_clock::now();
            std::unordered_map<std::string, OwnerAggregate> acc;
            const auto& parcels = layers[(size_t)parcel_layer_idx].features;
            const bool parcel_data_ready = !parcels.empty();
            const bool real_property_data_ready =
                real_property_layer_idx >= 0 &&
                (size_t)real_property_layer_idx < layers.size() &&
                !layers[(size_t)real_property_layer_idx].features.empty();
            if (!parcel_data_ready) {
                owner_sorted_mode = -1;
            } else {
            for (const auto& pf : parcels) {
                const LayerDef::FeatureGeom* rp = real_property_for_parcel(pf);
                std::string owner = owner_name_for(rp);
                if (owner.empty()) owner = owner_name_for(&pf);
                if (owner.empty()) continue;
                auto& row = acc[owner];
                if (row.owner.empty()) row.owner = owner;
                row.property_count += 1;
                row.area_m2 += parcel_area_sq_m(pf);
                row.value_usd += owner_value_for(rp);
            }
            owner_aggregates.clear();
            owner_aggregates.reserve(acc.size());
            for (auto& kv : acc) owner_aggregates.push_back(std::move(kv.second));
            if (!selected_owners.empty()) {
                std::unordered_set<std::string> owners_present;
                owners_present.reserve(owner_aggregates.size());
                for (const auto& row : owner_aggregates) owners_present.insert(row.owner);
                for (auto it = selected_owners.begin(); it != selected_owners.end();) {
                    if (owners_present.find(*it) == owners_present.end()) it = selected_owners.erase(it);
                    else ++it;
                }
            }
            owner_aggregates_dirty = !real_property_data_ready && owner_aggregates.empty();
            owner_sorted_mode = -1;
            }
            prof_owner_ms_last.store(prof_ms_since(owner_prof_begin), std::memory_order_relaxed);
        }
        if (!selected_owners.empty()) {
            std::vector<std::string> selected_owner_list(selected_owners.begin(), selected_owners.end());
            std::sort(selected_owner_list.begin(), selected_owner_list.end());
            uint64_t selection_key = 1469598103934665603ULL;
            for (const auto& owner : selected_owner_list) {
                selection_key ^= (uint64_t)owner.size();
                selection_key *= 1099511628211ULL;
                for (unsigned char ch : owner) {
                    selection_key ^= (uint64_t)ch;
                    selection_key *= 1099511628211ULL;
                }
            }
            uint64_t data_key = 1469598103934665603ULL;
            data_key ^= (uint64_t)owner_aggregates.size(); data_key *= 1099511628211ULL;
            data_key ^= (uint64_t)owner_cached_parcel_size; data_key *= 1099511628211ULL;
            data_key ^= (uint64_t)owner_cached_real_property_size; data_key *= 1099511628211ULL;
            data_key ^= (uint64_t)(parcel_vacancy_generation_applied + 3); data_key *= 1099511628211ULL;
            data_key ^= (uint64_t)(parcel_tax_generation_applied + 7); data_key *= 1099511628211ULL;
            if (!filtered_aggregate_snapshot.valid ||
                filtered_aggregate_snapshot.selection_key != selection_key ||
                filtered_aggregate_snapshot.data_key != data_key) {
                filtered_aggregate_snapshot = {};
                filtered_aggregate_snapshot.valid = true;
                filtered_aggregate_snapshot.selection_key = selection_key;
                filtered_aggregate_snapshot.data_key = data_key;
                for (const auto& row : owner_aggregates) {
                    if (selected_owners.find(row.owner) == selected_owners.end()) continue;
                    filtered_aggregate_snapshot.owner_property_count += row.property_count;
                    filtered_aggregate_snapshot.owner_area_m2 += row.area_m2;
                    filtered_aggregate_snapshot.owner_value_usd += row.value_usd;
                }
                if (parcel_layer_idx >= 0 && (size_t)parcel_layer_idx < layers.size()) {
                    const auto& pfeats = layers[(size_t)parcel_layer_idx].features;
                    for (size_t i = 0; i < pfeats.size(); ++i) {
                        const LayerDef::FeatureGeom* rp = real_property_for_parcel(pfeats[i]);
                        std::string owner = owner_name_for(rp);
                        if (owner.empty()) owner = owner_name_for(&pfeats[i]);
                        if (owner.empty() || selected_owners.find(owner) == selected_owners.end()) continue;
                        const int vac_notice = (i < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[i] : 0;
                        const int vac_rehab = (i < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[i] : 0;
                        if ((vac_notice + vac_rehab) <= 0) continue;
                        filtered_aggregate_snapshot.vacancy_parcels_matched++;
                        if (!pfeats[i].rings.empty()) filtered_aggregate_snapshot.vacancy_parcels_with_geometry++;
                    }
                }
            }
        } else {
            filtered_aggregate_snapshot = {};
        }

        const float right_panel_w = 360.0f;
        ImGui::SetNextWindowPos(ImVec2((float)w - right_panel_w - 12.0f, 12.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(right_panel_w, (float)h - 24.0f), ImGuiCond_Always);
        ImGui::Begin("Record Filters", nullptr, ImGuiWindowFlags_NoCollapse);
        if (ImGui::BeginTabBar("right_tabs")) {
        if (ImGui::BeginTabItem("Filters")) {
        const bool selected_valid =
            show_selected_parcel_details && parcel_layer_idx >= 0 &&
            (size_t)parcel_layer_idx < layers.size() &&
            selected_parcel_idx < layers[(size_t)parcel_layer_idx].features.size();
        const bool selected_zone_valid =
            show_selected_zone_details && zoning_layer_idx >= 0 &&
            (size_t)zoning_layer_idx < layers.size() &&
            selected_zone_idx < layers[(size_t)zoning_layer_idx].features.size();
        if (selected_valid) {
            const auto& selected = layers[(size_t)parcel_layer_idx].features[selected_parcel_idx];
            if (ImGui::Button("Back To Filters")) {
                show_selected_parcel_details = false;
                selected_parcel_idx = (size_t)-1;
            }
            ImGui::Separator();
            std::string blocklot_raw = getPropertyValue(selected, "BLOCKLOT");
            std::string blocklot = normalizeJoinKey(blocklot_raw);
            int vac_notice = (selected_parcel_idx < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[selected_parcel_idx] : 0;
            int vac_rehab = (selected_parcel_idx < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[selected_parcel_idx] : 0;
            int tax_lien = (selected_parcel_idx < parcel_tax_lien_by_feature.size()) ? parcel_tax_lien_by_feature[selected_parcel_idx] : 0;
            int tax_sale = (selected_parcel_idx < parcel_tax_sale_by_feature.size()) ? parcel_tax_sale_by_feature[selected_parcel_idx] : 0;
            double tax_lien_amount = (selected_parcel_idx < parcel_tax_lien_amount_by_feature.size()) ? parcel_tax_lien_amount_by_feature[selected_parcel_idx] : 0.0;
            double tax_sale_amount = (selected_parcel_idx < parcel_tax_sale_amount_by_feature.size()) ? parcel_tax_sale_amount_by_feature[selected_parcel_idx] : 0.0;
            ImGui::TextUnformatted("Parcel Details");
            ImGui::Separator();
            ImGui::Text("BLOCKLOT: %s", blocklot_raw.empty() ? "(none)" : blocklot_raw.c_str());
            ImGui::Text("Vacant Notices: %d", vac_notice);
            ImGui::Text("Vacant Rehab Records: %d", vac_rehab);
            ImGui::Text("Tax Lien Certificate Records: %d", tax_lien);
            if (tax_lien > 0) ImGui::Text("Tax Lien Total Amount: $%.2f", tax_lien_amount);
            ImGui::Text("Tax Sale 2021 Records: %d", tax_sale);
            if (tax_sale > 0) ImGui::Text("Tax Sale Total Lien: $%.2f", tax_sale_amount);
            const LayerDef::FeatureGeom* selected_rp = real_property_for_parcel(selected);
            draw_real_property_summary(selected_rp);
            ImGui::Separator();
            ImGui::BeginChild("selected_parcel_fields", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            draw_feature_properties("All Parcel Geometry Fields", selected);
            if (selected_rp) {
                ImGui::Separator();
                draw_feature_properties("All Real Property Fields", *selected_rp);
            }
            ImGui::EndChild();
        } else if (selected_zone_valid) {
            const auto& selected = layers[(size_t)zoning_layer_idx].features[selected_zone_idx];
            if (ImGui::Button("Back To Filters")) {
                show_selected_zone_details = false;
                selected_zone_idx = (size_t)-1;
            }
            ImGui::Separator();
            std::string zone_key = zoningClassKey(selected);
            std::string zone_label = zoningClassLabel(selected);
            std::string zone_description;
            auto meta_it = zoning_metadata.find(zone_key);
            if (meta_it != zoning_metadata.end()) {
                if (!meta_it->second.label.empty()) zone_label = meta_it->second.label;
                zone_description = meta_it->second.description;
            }
            ImVec4 zone_color = colorFromStableKey(zone_key);
            auto it_col = zoning_zone_color.find(zone_key);
            if (it_col != zoning_zone_color.end()) zone_color = it_col->second;
            ImGui::TextUnformatted("Zoning Details");
            ImGui::Separator();
            ImGui::ColorButton("##selected_zone_color", zone_color, ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
            ImGui::SameLine();
            ImGui::Text("Zone: %s", zone_key.empty() ? "(unlabeled)" : zone_key.c_str());
            if (!zone_label.empty() && zone_label != zone_key) {
                ImGui::TextWrapped("Label: %s", zone_label.c_str());
            }
            ImGui::TextWrapped("Description: %s", zone_description.empty() ? "No description available." : zone_description.c_str());
            ImGui::Separator();
            ImGui::TextUnformatted("All Zone Fields");
            ImGui::BeginChild("selected_zone_fields", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            for (const auto& kv : selected.properties) {
                if (kv.second.empty()) continue;
                ImGui::TextWrapped("%s: %s", kv.first.c_str(), kv.second.c_str());
            }
            ImGui::EndChild();
        } else {
            if (show_selected_parcel_details && !selected_valid) {
                show_selected_parcel_details = false;
                selected_parcel_idx = (size_t)-1;
            }
            if (show_selected_zone_details && !selected_zone_valid) {
                show_selected_zone_details = false;
                selected_zone_idx = (size_t)-1;
            }
            ImGui::Checkbox("Enable Filters", &filter_enabled);
            ImGui::Checkbox("Filter By Record Date", &filter_use_date);
            ImGui::BeginDisabled(!filter_enabled || !filter_use_date);
            ImGui::SliderInt("Year Min", &filter_year_min, 1900, 2100);
            ImGui::SliderInt("Year Max", &filter_year_max, 1900, 2100);
            if (filter_year_min > filter_year_max) std::swap(filter_year_min, filter_year_max);
            ImGui::EndDisabled();
            ImGui::SeparatorText("Common Fields");
            ImGui::InputText("Block/Lot", filter_blocklot, sizeof(filter_blocklot));
            ImGui::InputText("Status", filter_status, sizeof(filter_status));
            ImGui::InputText("Address", filter_address, sizeof(filter_address));
            ImGui::InputText("Owner", filter_owner, sizeof(filter_owner));
            ImGui::InputText("ZIP", filter_zip, sizeof(filter_zip));
            if (ImGui::Button("Clear Field Filters")) {
                filter_blocklot[0] = '\0';
                filter_status[0] = '\0';
                filter_address[0] = '\0';
                filter_owner[0] = '\0';
                filter_zip[0] = '\0';
                filter_use_date = false;
            }
            ImGui::SeparatorText("Record Year Histogram");
            auto first_prop_hist = [&](const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys) {
                for (const char* k : keys) {
                    std::string v = getPropertyValue(fg, k);
                    if (!v.empty()) return v;
                }
                return std::string();
            };
            if (hist_feature_counts.size() != layers.size()) {
                hist_feature_counts.assign(layers.size(), 0);
                hist_enabled.assign(layers.size(), false);
                hist_dirty = true;
            }
            for (size_t i = 0; i < layers.size(); ++i) {
                const size_t fc = layers[i].features.size();
                if (hist_feature_counts[i] != fc || hist_enabled[i] != layers[i].enabled) {
                    hist_feature_counts[i] = fc;
                    hist_enabled[i] = layers[i].enabled;
                    hist_dirty = true;
                }
            }
            if (hist_dirty) {
                std::fill(record_year_hist.begin(), record_year_hist.end(), 0);
                for (size_t li = 0; li < layers.size(); ++li) {
                    if (!layers[li].enabled) continue;
                    for (const auto& fg : layers[li].features) {
                        std::string ds = first_prop_hist(fg, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
                        if (ds.empty()) continue;
                        int y = extractYearMaybe(ds);
                        if (y < 1900 || y > 2100) continue;
                        record_year_hist[(size_t)(y - 1900)]++;
                    }
                }
                float max_bin = 1.0f;
                int nz_min = 2101;
                int nz_max = 1899;
                int nz_total = 0;
                for (size_t i = 0; i < record_year_hist.size(); ++i) {
                    record_year_hist_plot[i] = (float)record_year_hist[i];
                    if (record_year_hist[i] > 0) {
                        int y = 1900 + (int)i;
                        nz_min = std::min(nz_min, y);
                        nz_max = std::max(nz_max, y);
                        nz_total += record_year_hist[i];
                    }
                    if (record_year_hist_plot[i] > max_bin) max_bin = record_year_hist_plot[i];
                }
                record_year_hist_max_bin = max_bin;
                if (nz_min <= nz_max) {
                    record_year_nonzero_min = nz_min;
                    record_year_nonzero_max = nz_max;
                } else {
                    record_year_nonzero_min = 1900;
                    record_year_nonzero_max = 2100;
                }
                record_year_nonzero_total = nz_total;
                selected_record_year_dirty = true;
                hist_dirty = false;
            }
            ImGui::TextDisabled("Enabled-layer records by year");
            if (record_year_nonzero_total <= 0) {
                ImGui::TextDisabled("No recognized date fields found in currently enabled layers.");
            } else {
                ImGui::Text("Range: %d-%d  Total: %d  Peak: %.0f",
                            record_year_nonzero_min,
                            record_year_nonzero_max,
                            record_year_nonzero_total,
                            record_year_hist_max_bin);
                const int plot_offset = std::max(0, record_year_nonzero_min - 1900);
                const int plot_count = std::max(1, record_year_nonzero_max - record_year_nonzero_min + 1);
                ImGui::PlotHistogram(
                    "##record_year_hist",
                    record_year_hist_plot.data() + plot_offset,
                    plot_count,
                    0,
                    nullptr,
                    0.0f,
                    record_year_hist_max_bin * 1.05f,
                    ImVec2(-1.0f, 140.0f));
            }
            ImGui::BeginChild("year_hist_list", ImVec2(0, 130), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            for (int y = record_year_nonzero_min; y <= record_year_nonzero_max; ++y) {
                int c = record_year_hist[(size_t)(y - 1900)];
                if (c <= 0) continue;
                char label[64];
                std::snprintf(label, sizeof(label), "%d: %d records", y, c);
                if (ImGui::Selectable(label, selected_record_year == y)) {
                    selected_record_year = y;
                    selected_record_year_dirty = true;
                }
            }
            ImGui::EndChild();
            if (selected_record_year >= 1900 && selected_record_year <= 2100) {
                if (record_year_hist[(size_t)(selected_record_year - 1900)] <= 0) {
                    selected_record_year = -1;
                    selected_record_year_samples.clear();
                    selected_record_year_total = 0;
                    selected_record_year_dirty = false;
                }
            }
            if (selected_record_year >= 1900 && selected_record_year <= 2100 && selected_record_year_dirty) {
                constexpr size_t kMaxYearSamples = 8;
                selected_record_year_samples.clear();
                selected_record_year_total = 0;
                for (size_t li = 0; li < layers.size(); ++li) {
                    if (!layers[li].enabled) continue;
                    for (const auto& fg : layers[li].features) {
                        std::string ds = first_prop_hist(fg, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
                        if (ds.empty() || extractYearMaybe(ds) != selected_record_year) continue;
                        selected_record_year_total++;
                        if (selected_record_year_samples.size() >= kMaxYearSamples) continue;
                        std::string blocklot = first_prop_hist(fg, {"BLOCKLOT", "BLOCK_LOT", "LOT"});
                        std::string address = first_prop_hist(fg, {"FULLADDR", "PROPERTY_ADDRESS", "PREMISEADD", "ADDRESS", "Address", "ADDR"});
                        std::string owner = first_prop_hist(fg, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"});
                        std::string status = first_prop_hist(fg, {"STATUS", "STATE", "CASE_STATUS"});
                        std::ostringstream row;
                        row << layers[li].name << " | " << ds;
                        if (!blocklot.empty()) row << " | BL " << blocklot;
                        if (!address.empty()) row << " | " << address;
                        if (!owner.empty()) row << " | " << owner;
                        if (!status.empty()) row << " | " << status;
                        selected_record_year_samples.push_back(row.str());
                    }
                }
                selected_record_year_dirty = false;
            }
            if (selected_record_year >= 1900 && selected_record_year <= 2100) {
                ImGui::SeparatorText("Selected Year Records");
                ImGui::Text("%d: showing %zu of %d records",
                            selected_record_year,
                            selected_record_year_samples.size(),
                            selected_record_year_total);
                ImGui::BeginChild("selected_year_records", ImVec2(0, 170), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                if (selected_record_year_samples.empty()) {
                    ImGui::TextDisabled("No sample records available for this year.");
                } else {
                    for (const std::string& row : selected_record_year_samples) {
                        ImGui::TextWrapped("%s", row.c_str());
                        ImGui::Separator();
                    }
                }
                ImGui::EndChild();
            }
        }
        ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Vacancy-Parcel")) {
            const size_t notices_total = (size_t)cached_vac_notice_size;
            const size_t rehabs_total = (size_t)cached_vac_rehab_size;
            const size_t notices_matched = vacant_notice_rows_matched_total.load(std::memory_order_relaxed);
            const size_t rehabs_matched = vacant_rehab_rows_matched_total.load(std::memory_order_relaxed);
            size_t parcels_matched = vacant_parcels_matched_total.load(std::memory_order_relaxed);
            size_t parcels_geom = vacant_parcels_with_geometry_total.load(std::memory_order_relaxed);
            if (!selected_owners.empty() && filtered_aggregate_snapshot.valid) {
                parcels_matched = filtered_aggregate_snapshot.vacancy_parcels_matched;
                parcels_geom = filtered_aggregate_snapshot.vacancy_parcels_with_geometry;
            }
            ImGui::TextUnformatted("Vacant -> Parcel Join Quality");
            ImGui::Separator();
            ImGui::Text("Vacant notice records: %zu", notices_total);
            ImGui::Text("Matched to parcel: %zu (%.1f%%)", notices_matched, notices_total ? (100.0 * (double)notices_matched / (double)notices_total) : 0.0);
            ImGui::Text("Unmatched notices: %zu", notices_total >= notices_matched ? (notices_total - notices_matched) : 0);
            ImGui::Separator();
            ImGui::Text("Vacant rehab records: %zu", rehabs_total);
            ImGui::Text("Matched to parcel: %zu (%.1f%%)", rehabs_matched, rehabs_total ? (100.0 * (double)rehabs_matched / (double)rehabs_total) : 0.0);
            ImGui::Text("Unmatched rehabs: %zu", rehabs_total >= rehabs_matched ? (rehabs_total - rehabs_matched) : 0);
            ImGui::Separator();
            ImGui::Text("Parcels with vacancy evidence: %zu", parcels_matched);
            ImGui::Text("Those with parcel geometry: %zu", parcels_geom);
            if (!selected_owners.empty()) ImGui::TextDisabled("Counts scoped to selected owners.");
            ImGui::TextWrapped("Map styling uses parcel-level derived status. Raw vacant points are treated as child records.");
        ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Owners")) {
            ImGui::TextUnformatted("Owner Rankings");
            ImGui::Separator();
            const char* sort_items[] = {"# Properties", "Area Owned", "Value Owned"};
            ImGui::Combo("Sort By", &owner_sort_mode, sort_items, IM_ARRAYSIZE(sort_items));
            if (owner_sorted_mode != owner_sort_mode) {
                std::stable_sort(owner_aggregates.begin(), owner_aggregates.end(), [&](const OwnerAggregate& a, const OwnerAggregate& b) {
                    auto tie_break = [&]() {
                        if (a.property_count != b.property_count) return a.property_count > b.property_count;
                        if (std::abs(a.area_m2 - b.area_m2) > 0.5) return a.area_m2 > b.area_m2;
                        if (std::abs(a.value_usd - b.value_usd) > 0.5) return a.value_usd > b.value_usd;
                        return a.owner < b.owner;
                    };
                    if (owner_sort_mode == 1 && std::abs(a.area_m2 - b.area_m2) > 0.5) return a.area_m2 > b.area_m2;
                    if (owner_sort_mode == 2 && std::abs(a.value_usd - b.value_usd) > 0.5) return a.value_usd > b.value_usd;
                    if (owner_sort_mode == 0 && a.property_count != b.property_count) return a.property_count > b.property_count;
                    return tie_break();
                });
                owner_sorted_mode = owner_sort_mode;
            }
            ImGui::Text("Owners: %zu", owner_aggregates.size());
            ImGui::Text("Selected: %zu", selected_owners.size());
            ImGui::SameLine();
            if (ImGui::Button("Clear Selection")) {
                selected_owners.clear();
                selected_owner_anchor = -1;
            }
            if (!selected_owners.empty() && filtered_aggregate_snapshot.valid) {
                ImGui::TextDisabled("Filtered totals | properties: %zu | area: %.0f m^2 | value: $%.0f",
                                    filtered_aggregate_snapshot.owner_property_count,
                                    filtered_aggregate_snapshot.owner_area_m2,
                                    filtered_aggregate_snapshot.owner_value_usd);
            }
            if (owner_aggregates.empty()) {
                const size_t parcel_count =
                    (parcel_layer_idx >= 0 && (size_t)parcel_layer_idx < layers.size()) ? layers[(size_t)parcel_layer_idx].features.size() : 0;
                const size_t real_property_count =
                    (real_property_layer_idx >= 0 && (size_t)real_property_layer_idx < layers.size()) ? layers[(size_t)real_property_layer_idx].features.size() : 0;
                ImGui::TextDisabled("Waiting for owner data: parcels=%zu real_property=%zu", parcel_count, real_property_count);
            }
            ImGui::BeginChild("owner_rankings", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            const size_t max_rows = std::min<size_t>(500, owner_aggregates.size());
            for (size_t i = 0; i < max_rows; ++i) {
                const auto& r = owner_aggregates[i];
                const bool row_selected = selected_owners.find(r.owner) != selected_owners.end();
                ImGui::PushID((int)i);
                if (ImGui::Selectable(r.owner.c_str(), row_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    const bool shift = ImGui::GetIO().KeyShift;
                    const bool ctrl = ImGui::GetIO().KeyCtrl;
                    if (shift && selected_owner_anchor >= 0 && (size_t)selected_owner_anchor < max_rows) {
                        if (!ctrl) selected_owners.clear();
                        const size_t begin = std::min((size_t)selected_owner_anchor, i);
                        const size_t end = std::max((size_t)selected_owner_anchor, i);
                        for (size_t j = begin; j <= end; ++j) selected_owners.insert(owner_aggregates[j].owner);
                    } else if (ctrl) {
                        if (row_selected) selected_owners.erase(r.owner);
                        else selected_owners.insert(r.owner);
                        selected_owner_anchor = (int)i;
                    } else {
                        selected_owners.clear();
                        selected_owners.insert(r.owner);
                        selected_owner_anchor = (int)i;
                    }
                }
                ImGui::PopID();
                ImGui::TextDisabled("properties: %zu | area: %.0f m^2 | value: $%.0f", r.property_count, r.area_m2, r.value_usd);
                ImGui::Separator();
            }
            ImGui::EndChild();
        ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        }
        ImGui::End();

        const float map_x = 440.0f;
        const float map_w = std::max(260.0f, (float)w - map_x - right_panel_w - 24.0f);
        ImGui::SetNextWindowPos(ImVec2(map_x, 12), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(map_w, (float)h - 24.0f), ImGuiCond_Always);
        ImGui::Begin("Map", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::BeginTabBar("main_view_tabs")) {
        if (ImGui::BeginTabItem("Map")) {
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::GetContentRegionAvail();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImGui::InvisibleButton("map_canvas_input", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool map_hovered = ImGui::IsItemHovered();
        const bool map_active = ImGui::IsItemActive();

        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(242, 246, 250, 255));

        const int math_zoom = std::min(zoom, kMaxInternalMathZoom);
        const double zoom_scale = std::ldexp(1.0, zoom - math_zoom);
        auto wrap_world_x = [&](double x, int mz) -> double {
            const double period = 256.0 * (double)(1u << mz);
            x = std::fmod(x, period);
            if (x < 0.0) x += period;
            return x;
        };
        ImVec2 center_world = lonLatToWorldPx(center_lon, center_lat, math_zoom);
        center_world.x = (float)wrap_world_x((double)center_world.x, math_zoom);

        if (map_hovered) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                int next_zoom = std::clamp(zoom + (wheel > 0 ? 1 : -1), kMinZoom, kMaxZoom);
                if (next_zoom != zoom) {
                    const ImVec2 mouse = ImGui::GetIO().MousePos;
                    ImVec2 mouse_world = ImVec2(
                        center_world.x + (float)((mouse.x - (origin.x + size.x * 0.5f)) / zoom_scale),
                        center_world.y + (float)((mouse.y - (origin.y + size.y * 0.5f)) / zoom_scale));
                    mouse_world.x = (float)wrap_world_x((double)mouse_world.x, math_zoom);
                    ImVec2 ll = worldPxToLonLat(mouse_world, math_zoom);
                    zoom = next_zoom;
                    const int next_math_zoom = std::min(zoom, kMaxInternalMathZoom);
                    const double next_zoom_scale = std::ldexp(1.0, zoom - next_math_zoom);
                    ImVec2 mouse_world_new = lonLatToWorldPx(ll.x, ll.y, next_math_zoom);
                    center_world = ImVec2(
                        mouse_world_new.x - (float)((mouse.x - (origin.x + size.x * 0.5f)) / next_zoom_scale),
                        mouse_world_new.y - (float)((mouse.y - (origin.y + size.y * 0.5f)) / next_zoom_scale));
                    center_world.x = (float)wrap_world_x((double)center_world.x, next_math_zoom);
                    ImVec2 cll = worldPxToLonLat(center_world, next_math_zoom);
                    center_lon = cll.x;
                    center_lat = std::clamp((double)cll.y, -85.0, 85.0);
                }
            }

            if (map_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                ImVec2 d = ImGui::GetIO().MouseDelta;
                center_world.x -= (float)(d.x / zoom_scale);
                center_world.y -= (float)(d.y / zoom_scale);
                center_world.x = (float)wrap_world_x((double)center_world.x, math_zoom);
                ImVec2 ll = worldPxToLonLat(center_world, math_zoom);
                center_lon = ll.x;
                center_lat = std::clamp((double)ll.y, -85.0, 85.0);
            }
        }

        center_world = lonLatToWorldPx(center_lon, center_lat, math_zoom);
        center_world.x = (float)wrap_world_x((double)center_world.x, math_zoom);
        const float screen_cx = origin.x + size.x * 0.5f;
        const float screen_cy = origin.y + size.y * 0.5f;
        const float zsf = (float)zoom_scale;
        auto project_world = [&](const ImVec2& wp) -> ImVec2 {
            return ImVec2(
                screen_cx + (wp.x - center_world.x) * zsf,
                screen_cy + (wp.y - center_world.y) * zsf);
        };
        const ImVec2 mouse_screen = ImGui::GetIO().MousePos;
        const ImVec2 mouse_world(
            center_world.x + (float)((mouse_screen.x - (origin.x + size.x * 0.5f)) / zoom_scale),
            center_world.y + (float)((mouse_screen.y - (origin.y + size.y * 0.5f)) / zoom_scale));
        const ImVec2 mouse_ll = worldPxToLonLat(mouse_world, math_zoom);
        static ImVec2 context_ll(0.0f, 0.0f);
        if (map_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            context_ll = mouse_ll;
            ImGui::OpenPopup("map_context_menu");
        }
        if (ImGui::BeginPopup("map_context_menu")) {
            ImGui::Text("Lon: %.6f", context_ll.x);
            ImGui::Text("Lat: %.6f", context_ll.y);
            ImGui::Separator();
            if (ImGui::MenuItem("Open Google Maps Street View")) {
                std::ostringstream url;
                url << std::fixed << std::setprecision(7)
                    << "https://www.google.com/maps/@?api=1&map_action=pano&viewpoint="
                    << context_ll.y << "," << context_ll.x;
                openUrlInBrowser(url.str());
            }
            ImGui::EndPopup();
        }
        const LayerDef::FeatureGeom* hovered_parcel = nullptr;
        size_t hovered_parcel_idx = (size_t)-1;
        const LayerDef::FeatureGeom* hovered_zone = nullptr;
        size_t hovered_zone_idx = (size_t)-1;
        const double half_w_world = (size.x * 0.5) / zoom_scale;
        const double half_h_world = (size.y * 0.5) / zoom_scale;
        ImVec2 ll_a = worldPxToLonLat(ImVec2(center_world.x - (float)half_w_world, center_world.y - (float)half_h_world), math_zoom);
        ImVec2 ll_b = worldPxToLonLat(ImVec2(center_world.x + (float)half_w_world, center_world.y + (float)half_h_world), math_zoom);
        const float view_min_lon = std::min(ll_a.x, ll_b.x);
        const float view_max_lon = std::max(ll_a.x, ll_b.x);
        const float view_min_lat = std::min(ll_a.y, ll_b.y);
        const float view_max_lat = std::max(ll_a.y, ll_b.y);
        auto layer_hover_active = [&](int idx) -> bool {
            return idx >= 0 && (size_t)idx < layer_hover_enabled.size() && layer_hover_enabled[(size_t)idx];
        };
        auto layer_inspect_active = [&](int idx) -> bool {
            return idx >= 0 && (size_t)idx < layer_inspect_enabled.size() && layer_inspect_enabled[(size_t)idx];
        };
        const bool parcel_hover_active = hover_inspector_enabled && layer_hover_active(parcel_layer_idx);
        const bool parcel_inspect_active = layer_inspect_active(parcel_layer_idx);
        const bool zoning_hover_active = hover_inspector_enabled && layer_hover_active(zoning_layer_idx);
        const bool zoning_inspect_active = layer_inspect_active(zoning_layer_idx);

        if (map_hovered && (parcel_hover_active || parcel_inspect_active) && parcel_layer_idx >= 0) {
            const size_t pli = (size_t)parcel_layer_idx;
            if (pli < layers.size() && pli < layer_spatial.size() && layer_spatial[pli].built) {
                std::vector<uint32_t> hover_candidates;
                if (queryLayerSpatialIndex(
                        layer_spatial[pli],
                        mouse_ll.x,
                        mouse_ll.y,
                        mouse_ll.x,
                        mouse_ll.y,
                        hover_candidates)) {
                    float best_area = std::numeric_limits<float>::infinity();
                    for (uint32_t fidx : hover_candidates) {
                        if (fidx >= layers[pli].features.size()) continue;
                        const auto& fg = layers[pli].features[(size_t)fidx];
                        if (fg.rings.empty()) continue;
                        if (fg.extent.max_lon < view_min_lon || fg.extent.min_lon > view_max_lon ||
                            fg.extent.max_lat < view_min_lat || fg.extent.min_lat > view_max_lat) {
                            continue;
                        }
                        if (mouse_ll.x < fg.extent.min_lon || mouse_ll.x > fg.extent.max_lon ||
                            mouse_ll.y < fg.extent.min_lat || mouse_ll.y > fg.extent.max_lat) {
                            continue;
                        }
                        if (!pointInFeature(fg, mouse_ll.x, mouse_ll.y)) continue;
                        const float area = std::max(0.0f, fg.extent.max_lon - fg.extent.min_lon) *
                                           std::max(0.0f, fg.extent.max_lat - fg.extent.min_lat);
                        if (area < best_area) {
                            best_area = area;
                            hovered_parcel = &fg;
                            hovered_parcel_idx = (size_t)fidx;
                        }
                    }
                }
            }
        }
        if (map_hovered && (zoning_hover_active || zoning_inspect_active) && zoning_layer_idx >= 0 && (size_t)zoning_layer_idx < layers.size()) {
            const size_t zli = (size_t)zoning_layer_idx;
            if (zli < layer_spatial.size() && layer_spatial[zli].built) {
                std::vector<uint32_t> zone_candidates;
                if (queryLayerSpatialIndex(
                        layer_spatial[zli],
                        mouse_ll.x,
                        mouse_ll.y,
                        mouse_ll.x,
                        mouse_ll.y,
                        zone_candidates)) {
                    float best_area = std::numeric_limits<float>::infinity();
                    const auto& zfeats = layers[zli].features;
                    for (uint32_t zidx : zone_candidates) {
                        if (zidx >= zfeats.size()) continue;
                        const auto& zf = zfeats[zidx];
                        if (zf.rings.empty()) continue;
                        if (mouse_ll.x < zf.extent.min_lon || mouse_ll.x > zf.extent.max_lon ||
                            mouse_ll.y < zf.extent.min_lat || mouse_ll.y > zf.extent.max_lat) {
                            continue;
                        }
                        if (!pointInFeature(zf, mouse_ll.x, mouse_ll.y)) continue;
                        const float area = std::max(0.0f, zf.extent.max_lon - zf.extent.min_lon) *
                                           std::max(0.0f, zf.extent.max_lat - zf.extent.min_lat);
                        if (area < best_area) {
                            best_area = area;
                            hovered_zone = &zf;
                            hovered_zone_idx = (size_t)zidx;
                        }
                    }
                }
            }
        }
        int min_x = (int)std::floor((center_world.x - half_w_world) / 256.0) - 1;
        int max_x = (int)std::floor((center_world.x + half_w_world) / 256.0) + 1;
        int min_y = (int)std::floor((center_world.y - half_h_world) / 256.0) - 1;
        int max_y = (int)std::floor((center_world.y + half_h_world) / 256.0) + 1;
        const int period = 1 << math_zoom;
        const int max_tile = period - 1;

        const auto tile_prof_begin = std::chrono::steady_clock::now();
        for (int tx = min_x; tx <= max_x; ++tx) {
            for (int ty = min_y; ty <= max_y; ++ty) {
                int wrapped_x = tx;
                while (wrapped_x < 0) wrapped_x += period;
                while (wrapped_x > max_tile) wrapped_x -= period;
                if (ty < 0 || ty > max_tile) continue;

                TileSample sample = getTileSample(root, math_zoom, wrapped_x, ty);
                if (!sample.tex) continue;

                ImVec2 tile_world((float)(tx * 256), (float)(ty * 256));
                ImVec2 p0 = project_world(tile_world);
                ImVec2 p1(p0.x + (float)(256.0 * zoom_scale), p0.y + (float)(256.0 * zoom_scale));
                draw->AddImage(
                    (ImTextureID)sample.tex->descriptor,
                    p0,
                    p1,
                    sample.uv0,
                    sample.uv1,
                    app_settings.grayscale_basemap ? IM_COL32(178, 178, 178, 255) : IM_COL32_WHITE);
                if (app_settings.grayscale_basemap) {
                    draw->AddRectFilled(p0, p1, IM_COL32(244, 244, 244, 78));
                }
                prof_tiles_drawn_frame++;
            }
        }
        prof_tile_ms_last.store(prof_ms_since(tile_prof_begin), std::memory_order_relaxed);

        bool vacant_notice_enabled = vacant_notice_layer_idx >= 0 && layers[(size_t)vacant_notice_layer_idx].enabled;
        bool vacant_rehab_enabled = vacant_rehab_layer_idx >= 0 && layers[(size_t)vacant_rehab_layer_idx].enabled;
        bool tax_lien_enabled = tax_lien_layer_idx >= 0 && layers[(size_t)tax_lien_layer_idx].enabled;
        bool tax_sale_enabled = tax_sale_layer_idx >= 0 && layers[(size_t)tax_sale_layer_idx].enabled;
        auto color_with_alpha = [](const ImVec4& c, int alpha) -> ImU32 {
            const int r = std::clamp((int)std::lround(c.x * 255.0f), 0, 255);
            const int g = std::clamp((int)std::lround(c.y * 255.0f), 0, 255);
            const int b = std::clamp((int)std::lround(c.z * 255.0f), 0, 255);
            return IM_COL32(r, g, b, alpha);
        };
        auto darken_color = [](const ImVec4& c, float mul) -> ImVec4 {
            return ImVec4(std::clamp(c.x * mul, 0.0f, 1.0f),
                          std::clamp(c.y * mul, 0.0f, 1.0f),
                          std::clamp(c.z * mul, 0.0f, 1.0f), 1.0f);
        };
        auto vacancy_base_color = [&](int vac_notice, int vac_rehab) -> ImVec4 {
            const ImVec4 notice_c = (vacant_notice_layer_idx >= 0) ? layers[(size_t)vacant_notice_layer_idx].color : ImVec4(1, 0, 0, 1);
            const ImVec4 rehab_c = (vacant_rehab_layer_idx >= 0) ? layers[(size_t)vacant_rehab_layer_idx].color : ImVec4(0, 1, 1, 1);
            if (vac_notice > 0 && vac_rehab > 0) {
                return ImVec4((notice_c.x + rehab_c.x) * 0.5f,
                              (notice_c.y + rehab_c.y) * 0.5f,
                              (notice_c.z + rehab_c.z) * 0.5f, 1.0f);
            }
            if (vac_rehab > 0) return rehab_c;
            return notice_c;
        };
        const int ring_step = lodRingStepForZoom(math_zoom);
        const bool allow_parcel_scale_fill = math_zoom >= 14;
        auto is_parcel_scale_layer = [&](size_t layer_idx) -> bool {
            return ((int)layer_idx == parcel_layer_idx ||
                    (int)layer_idx == real_property_layer_idx ||
                    (int)layer_idx == vacant_notice_layer_idx ||
                    (int)layer_idx == vacant_rehab_layer_idx ||
                    (int)layer_idx == tax_lien_layer_idx ||
                    (int)layer_idx == tax_sale_layer_idx);
        };
        auto should_fill_layer_polygon = [&](size_t layer_idx) -> bool {
            // Large-area layers still need low-zoom fill; parcel-scale fills are the expensive case.
            return !is_parcel_scale_layer(layer_idx) || allow_parcel_scale_fill;
        };
        std::vector<std::vector<ImVec2>> scratch_world_rings;
        std::pair<ImVec2, ImVec2> scratch_world_extent;
        auto get_world_rings = [&](size_t, uint32_t, const LayerDef::FeatureGeom& fg)
            -> const std::vector<std::vector<ImVec2>>& {
            scratch_world_rings.clear();
            scratch_world_rings.reserve(fg.rings.size());
            for (const auto& r : fg.rings) {
                std::vector<ImVec2> rr;
                rr.reserve(r.size());
                for (const ImVec2& ll : r) rr.push_back(lonLatToWorldPx(ll.x, ll.y, math_zoom));
                scratch_world_rings.push_back(std::move(rr));
            }
            return scratch_world_rings;
        };
        auto get_world_extent = [&](size_t, uint32_t, const LayerDef::FeatureGeom& fg)
            -> const std::pair<ImVec2, ImVec2>& {
            ImVec2 p0w = lonLatToWorldPx(fg.extent.min_lon, fg.extent.max_lat, math_zoom);
            ImVec2 p1w = lonLatToWorldPx(fg.extent.max_lon, fg.extent.min_lat, math_zoom);
            scratch_world_extent = std::make_pair(p0w, p1w);
            return scratch_world_extent;
        };
        std::vector<ImVec2> scratch_fill_verts;
        scratch_fill_verts.reserve(4096);
        std::vector<uint32_t> scratch_fill_indices;
        scratch_fill_indices.reserve(12288);
        std::vector<ImVec2> scratch_line;
        scratch_line.reserve(1024);
        size_t fill_attempts_frame = 0;
        size_t fill_success_frame = 0;
        size_t fill_no_triangles_frame = 0;
        size_t fill_bad_indices_frame = 0;
        auto project_world_rings_for_fill = [&](const std::vector<std::vector<ImVec2>>& world_rings) -> size_t {
            size_t total = 0;
            for (const auto& r : world_rings) total += r.size();
            scratch_fill_verts.clear();
            scratch_fill_verts.reserve(total);
            for (const auto& r : world_rings) {
                for (const ImVec2& wp : r) scratch_fill_verts.push_back(project_world(wp));
            }
            return total;
        };
        auto append_world_ring_line = [&](const std::vector<ImVec2>& world_ring) {
            scratch_line.clear();
            if (world_ring.empty()) return;
            const int step = std::max(1, ring_step);
            const size_t n = world_ring.size();
            if (step == 1 || n <= 4) {
                for (const ImVec2& wp : world_ring) scratch_line.push_back(project_world(wp));
            } else {
                scratch_line.reserve((n / (size_t)step) + 2);
                for (size_t i = 0; i < n; i += (size_t)step) scratch_line.push_back(project_world(world_ring[i]));
                if ((n - 1) % (size_t)step != 0) scratch_line.push_back(project_world(world_ring.back()));
            }
        };
        auto draw_tessellated_fill = [&](const LayerDef::FeatureGeom& fg,
                                         const std::vector<std::vector<ImVec2>>& world_rings,
                                         ImU32 fill_color) -> bool {
            fill_attempts_frame++;
            if (fg.triangles.empty()) {
                fill_no_triangles_frame++;
                return false;
            }
            const size_t vcount = project_world_rings_for_fill(world_rings);
            if (vcount < 3) return false;

            scratch_fill_indices.clear();
            scratch_fill_indices.reserve(fg.triangles.size());
            for (size_t ti = 0; ti + 2 < fg.triangles.size(); ti += 3) {
                const uint32_t a = fg.triangles[ti + 0];
                const uint32_t b = fg.triangles[ti + 1];
                const uint32_t cidx = fg.triangles[ti + 2];
                if (a < vcount && b < vcount && cidx < vcount) {
                    scratch_fill_indices.push_back(a);
                    scratch_fill_indices.push_back(b);
                    scratch_fill_indices.push_back(cidx);
                }
            }
            if (scratch_fill_indices.empty()) {
                fill_bad_indices_frame++;
                return false;
            }

            const ImDrawListFlags old_flags = draw->Flags;
            draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            for (size_t ii = 0; ii + 2 < scratch_fill_indices.size(); ii += 3) {
                draw->AddTriangleFilled(
                    scratch_fill_verts[scratch_fill_indices[ii + 0]],
                    scratch_fill_verts[scratch_fill_indices[ii + 1]],
                    scratch_fill_verts[scratch_fill_indices[ii + 2]],
                    fill_color);
            }
            draw->Flags = old_flags;
            fill_success_frame++;
            return true;
        };
        auto first_prop = [&](const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys) {
            for (const char* k : keys) {
                std::string v = getPropertyValue(fg, k);
                if (!v.empty()) return v;
            }
            return std::string();
        };
        auto is_crime_layer = [&](size_t layer_idx) -> bool {
            return (crime_nibrs_layer_idx >= 0 && (int)layer_idx == crime_nibrs_layer_idx) ||
                   (crime_legacy_layer_idx >= 0 && (int)layer_idx == crime_legacy_layer_idx);
        };
        auto crime_feature_matches = [&](const LayerDef::FeatureGeom& fg) -> bool {
            if (!crime_filter_enabled) return true;
            const std::string desc = toLowerAscii(first_prop(fg, {"Description", "description", "OFFENSE", "UCRDescription"}));
            const std::string code = toLowerAscii(first_prop(fg, {"CrimeCode", "UCR_CODE", "UCRCode"}));
            const std::string dt = first_prop(fg, {"CrimeDateTime", "CrimeDate", "DATE", "RECORD_DATE"});
            if (crime_filter_use_year) {
                int yr = extractYearMaybe(dt);
                if (yr < 0 || yr < crime_year_min || yr > crime_year_max) return false;
            }
            const bool any_type =
                crime_filter_homicide || crime_filter_robbery || crime_filter_assault ||
                crime_filter_burglary || crime_filter_theft || crime_filter_auto_theft || crime_filter_drug || crime_filter_shooting;
            if (!any_type) return true;
            auto has = [&](const char* s) { return desc.find(s) != std::string::npos || code.find(s) != std::string::npos; };
            bool ok = false;
            if (crime_filter_homicide && (has("homicide") || has("murder"))) ok = true;
            if (crime_filter_robbery && has("robbery")) ok = true;
            if (crime_filter_assault && (has("assault") || has("aggravated assault"))) ok = true;
            if (crime_filter_burglary && has("burglary")) ok = true;
            if (crime_filter_theft && (has("larceny") || has("theft"))) ok = true;
            if (crime_filter_auto_theft && (has("motor vehicle theft") || has("auto theft") || has("vehicle theft"))) ok = true;
            if (crime_filter_drug && (has("drug") || has("narcotic"))) ok = true;
            if (crime_filter_shooting && has("shooting")) ok = true;
            return ok;
        };
        auto feature_passes_filters = [&](size_t layer_idx, size_t feature_idx, const LayerDef::FeatureGeom& fg) -> bool {
            if (is_crime_layer(layer_idx)) {
                if (!filter_enabled && !crime_filter_enabled) return true;
                return crime_feature_matches(fg);
            }
            const bool selected_owner_filter_active =
                !selected_owners.empty() &&
                parcel_layer_idx >= 0 &&
                (int)layer_idx == parcel_layer_idx;
            if (!filter_enabled && !selected_owner_filter_active) return true;
            const LayerDef::FeatureGeom* rp_join = nullptr;
            if (parcel_layer_idx >= 0 &&
                (int)layer_idx == parcel_layer_idx &&
                real_property_layer_idx >= 0 &&
                (size_t)real_property_layer_idx < layers.size()) {
                std::string bl = normalizeJoinKey(first_prop(fg, {"BLOCKLOT", "BLOCK_LOT", "LOT"}));
                if (!bl.empty()) {
                    auto itrp = real_property_by_blocklot.find(bl);
                    if (itrp != real_property_by_blocklot.end() &&
                        itrp->second < layers[(size_t)real_property_layer_idx].features.size()) {
                        rp_join = &layers[(size_t)real_property_layer_idx].features[itrp->second];
                    }
                }
            }
            if (selected_owner_filter_active) {
                std::string owner = owner_name_for(rp_join);
                if (owner.empty()) owner = owner_name_for(&fg);
                if (owner.empty() || selected_owners.find(owner) == selected_owners.end()) return false;
            }
            if (!filter_enabled) return true;
            if (filter_use_date) {
                std::string ds = first_prop(fg, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
                if (ds.empty() && rp_join) ds = first_prop(*rp_join, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
                if (ds.empty()) return false;
                int yr = extractYearMaybe(ds);
                if (yr < 0 || yr < filter_year_min || yr > filter_year_max) return false;
            }
            const std::string q_blocklot(filter_blocklot);
            const std::string q_status(filter_status);
            const std::string q_address(filter_address);
            const std::string q_owner(filter_owner);
            const std::string q_zip(filter_zip);
            if (!q_blocklot.empty()) {
                std::string bl = first_prop(fg, {"BLOCKLOT", "BLOCK_LOT", "LOT"});
                if (!containsCaseInsensitive(bl, q_blocklot)) return false;
            }
            if (!q_status.empty()) {
                std::string st = first_prop(fg, {"STATUS", "STATE", "CASE_STATUS"});
                if (st.empty() && rp_join) st = first_prop(*rp_join, {"STATUS", "STATE", "CASE_STATUS"});
                if (st.empty() && parcel_layer_idx >= 0 && (int)layer_idx == parcel_layer_idx) {
                    const int vn = (feature_idx < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[feature_idx] : 0;
                    const int vr = (feature_idx < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[feature_idx] : 0;
                    st = (vn + vr) > 0 ? "vacant" : "occupied";
                }
                if (!containsCaseInsensitive(st, q_status)) return false;
            }
            if (!q_address.empty()) {
                std::string ad = first_prop(fg, {"FULLADDR", "PROPERTY_ADDRESS", "PREMISEADD", "ADDRESS", "Address", "ADDR"});
                if (ad.empty() && rp_join) ad = first_prop(*rp_join, {"FULLADDR", "PROPERTY_ADDRESS", "PREMISEADD", "ADDRESS", "Address", "ADDR"});
                if (!containsCaseInsensitive(ad, q_address)) return false;
            }
            if (!q_owner.empty()) {
                std::string ow = first_prop(fg, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"});
                if (ow.empty() && rp_join) ow = first_prop(*rp_join, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"});
                if (!containsCaseInsensitive(ow, q_owner)) return false;
            }
            if (!q_zip.empty()) {
                std::string zp = first_prop(fg, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
                if (zp.empty() && rp_join) zp = first_prop(*rp_join, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
                if (!containsCaseInsensitive(zp, q_zip)) return false;
            }
            return true;
        };
        auto try_get_prop_float = [&](const LayerDef::FeatureGeom& fg, const std::string& key, float& out) -> bool {
            if (key.empty()) return false;
            for (const auto& kv : fg.properties) {
                if (kv.first != key) continue;
                char* end = nullptr;
                const float v = std::strtof(kv.second.c_str(), &end);
                if (end == kv.second.c_str() || (end && *end != '\0')) return false;
                if (!std::isfinite(v)) return false;
                out = v;
                return true;
            }
            return false;
        };
        auto heat_color = [&](float t) -> ImVec4 {
            t = std::clamp(t, 0.0f, 1.0f);
            const ImVec4 cold(0.12f, 0.35f, 0.75f, 1.0f);
            const ImVec4 mid(0.98f, 0.83f, 0.26f, 1.0f);
            const ImVec4 hot(0.82f, 0.14f, 0.12f, 1.0f);
            auto lerp = [](const ImVec4& a, const ImVec4& b, float u) {
                return ImVec4(
                    a.x + (b.x - a.x) * u,
                    a.y + (b.y - a.y) * u,
                    a.z + (b.z - a.z) * u,
                    a.w + (b.w - a.w) * u);
            };
            if (t < 0.5f) return lerp(cold, mid, t * 2.0f);
            return lerp(mid, hot, (t - 0.5f) * 2.0f);
        };
        auto hash_combine_u64 = [](uint64_t& seed, uint64_t v) {
            seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        };
        auto hash_combine_f = [&](uint64_t& seed, float v) {
            uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            hash_combine_u64(seed, (uint64_t)bits);
        };
        auto hash_combine_qd = [&](uint64_t& seed, double v, double scale) {
            if (!std::isfinite(v)) {
                hash_combine_u64(seed, 0xffffffffffffffffULL);
                return;
            }
            const int64_t q = (int64_t)std::llround(v * scale);
            hash_combine_u64(seed, (uint64_t)q);
        };
        const float global_heat_cell = std::max(4.0f, global_heat_cell_px);
        std::vector<HeatSample> heat_samples;
        auto resolve_layer_heat_settings = [&](size_t layer_idx, HeatSample& hs) {
            const bool use_global = layer_idx >= layer_heatmap_use_global_settings.size() || layer_heatmap_use_global_settings[layer_idx];
            hs.layer = (int)layer_idx;
            hs.algo = (layer_idx < layer_heatmap_algo.size() && layer_heatmap_algo[layer_idx] >= 0) ? layer_heatmap_algo[layer_idx] : heatmap_algo;
            hs.cell_px = std::max(4.0f, use_global || layer_idx >= layer_heatmap_cell_px.size() ? global_heat_cell_px : layer_heatmap_cell_px[layer_idx]);
            hs.bandwidth_px = std::max(1.0f, use_global || layer_idx >= layer_heatmap_bandwidth_px.size() ? heatmap_bandwidth_px : layer_heatmap_bandwidth_px[layer_idx]);
            hs.blur_sigma_px = std::max(0.0f, use_global || layer_idx >= layer_heatmap_blur_sigma_px.size() ? heatmap_blur_sigma_px : layer_heatmap_blur_sigma_px[layer_idx]);
            hs.percentile_clip = std::clamp(use_global || layer_idx >= layer_heatmap_percentile_clip.size() ? heatmap_percentile_clip : layer_heatmap_percentile_clip[layer_idx], 50.0f, 100.0f);
            hs.zoom_adaptive_bandwidth = use_global || layer_idx >= layer_heatmap_zoom_adaptive_bandwidth.size() ? heatmap_zoom_adaptive_bandwidth : layer_heatmap_zoom_adaptive_bandwidth[layer_idx];
            hs.multires_enabled = use_global || layer_idx >= layer_heatmap_multires_enabled.size() ? heatmap_multires_enabled : layer_heatmap_multires_enabled[layer_idx];
            hs.multires_blend = std::clamp(use_global || layer_idx >= layer_heatmap_multires_blend.size() ? heatmap_multires_blend : layer_heatmap_multires_blend[layer_idx], 0.0f, 1.0f);
        };
        bool smooth_only_heatmap = true;
        bool any_active_heatmap = false;
        for (size_t i = 0; i < layers.size(); ++i) {
            const bool active_heatmap =
                layers[i].enabled &&
                i < layer_heatmap_enabled.size() &&
                i < layer_heatmap_max_zoom.size() &&
                layer_heatmap_enabled[i] &&
                zoom <= layer_heatmap_max_zoom[i];
            if (!active_heatmap) continue;
            any_active_heatmap = true;
            const int algo = (i < layer_heatmap_algo.size() && layer_heatmap_algo[i] >= 0) ? layer_heatmap_algo[i] : heatmap_algo;
            if (algo != 1 && algo != 2 && algo != 4) smooth_only_heatmap = false;
        }
        if (!any_active_heatmap) smooth_only_heatmap = false;
        uint64_t heatmap_key = 1469598103934665603ULL;
        hash_combine_u64(heatmap_key, (uint64_t)zoom);
        hash_combine_u64(heatmap_key, (uint64_t)math_zoom);
        hash_combine_qd(heatmap_key, center_lon, 10000000.0);
        hash_combine_qd(heatmap_key, center_lat, 10000000.0);
        hash_combine_qd(heatmap_key, view_min_lon, 10000000.0);
        hash_combine_qd(heatmap_key, view_min_lat, 10000000.0);
        hash_combine_qd(heatmap_key, view_max_lon, 10000000.0);
        hash_combine_qd(heatmap_key, view_max_lat, 10000000.0);
        hash_combine_u64(heatmap_key, (uint64_t)std::max(1, (int)std::lround(size.x)));
        hash_combine_u64(heatmap_key, (uint64_t)std::max(1, (int)std::lround(size.y)));
        hash_combine_f(heatmap_key, global_heat_cell_px);
        hash_combine_u64(heatmap_key, (uint64_t)heatmap_algo);
        hash_combine_u64(heatmap_key, (uint64_t)heatmap_quality_preset);
        hash_combine_f(heatmap_key, heatmap_bandwidth_px);
        hash_combine_f(heatmap_key, heatmap_blur_sigma_px);
        hash_combine_f(heatmap_key, heatmap_percentile_clip);
        hash_combine_u64(heatmap_key, heatmap_zoom_adaptive_bandwidth ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, heatmap_multires_enabled ? 1ULL : 0ULL);
        hash_combine_f(heatmap_key, heatmap_multires_blend);
        hash_combine_u64(heatmap_key, filter_enabled ? 1ULL : 0ULL);
        for (const char* p = filter_blocklot; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        for (const char* p = filter_status; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        for (const char* p = filter_address; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        for (const char* p = filter_owner; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        if (!selected_owners.empty()) {
            std::vector<std::string> selected_owner_list(selected_owners.begin(), selected_owners.end());
            std::sort(selected_owner_list.begin(), selected_owner_list.end());
            for (const auto& owner : selected_owner_list) {
                hash_combine_u64(heatmap_key, (uint64_t)owner.size());
                for (unsigned char ch : owner) hash_combine_u64(heatmap_key, (uint64_t)ch);
            }
        }
        for (const char* p = filter_zip; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        hash_combine_u64(heatmap_key, filter_use_date ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, (uint64_t)filter_year_min);
        hash_combine_u64(heatmap_key, (uint64_t)filter_year_max);
        hash_combine_u64(heatmap_key, crime_filter_enabled ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_use_year ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, (uint64_t)crime_year_min);
        hash_combine_u64(heatmap_key, (uint64_t)crime_year_max);
        hash_combine_u64(heatmap_key, crime_filter_homicide ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_robbery ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_assault ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_burglary ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_theft ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_auto_theft ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_drug ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_shooting ? 1ULL : 0ULL);
        for (size_t i = 0; i < layers.size(); ++i) {
            hash_combine_u64(heatmap_key, layers[i].enabled ? 1ULL : 0ULL);
            hash_combine_u64(heatmap_key, (i < layer_heatmap_enabled.size() && layer_heatmap_enabled[i]) ? 1ULL : 0ULL);
            hash_combine_u64(heatmap_key, (uint64_t)((i < layer_heatmap_max_zoom.size()) ? layer_heatmap_max_zoom[i] : 13));
            hash_combine_u64(heatmap_key, (i < layer_heatmap_use_gradient.size() && layer_heatmap_use_gradient[i]) ? 1ULL : 0ULL);
            hash_combine_u64(heatmap_key, (uint64_t)((i < layer_heatmap_algo.size()) ? (layer_heatmap_algo[i] + 1) : 0));
            const bool use_global = i >= layer_heatmap_use_global_settings.size() || layer_heatmap_use_global_settings[i];
            hash_combine_u64(heatmap_key, use_global ? 1ULL : 0ULL);
            if (!use_global) {
                hash_combine_f(heatmap_key, i < layer_heatmap_cell_px.size() ? layer_heatmap_cell_px[i] : global_heat_cell_px);
                hash_combine_f(heatmap_key, i < layer_heatmap_bandwidth_px.size() ? layer_heatmap_bandwidth_px[i] : heatmap_bandwidth_px);
                hash_combine_f(heatmap_key, i < layer_heatmap_blur_sigma_px.size() ? layer_heatmap_blur_sigma_px[i] : heatmap_blur_sigma_px);
                hash_combine_f(heatmap_key, i < layer_heatmap_percentile_clip.size() ? layer_heatmap_percentile_clip[i] : heatmap_percentile_clip);
                hash_combine_u64(heatmap_key, (i < layer_heatmap_zoom_adaptive_bandwidth.size() && layer_heatmap_zoom_adaptive_bandwidth[i]) ? 1ULL : 0ULL);
                hash_combine_u64(heatmap_key, (i < layer_heatmap_multires_enabled.size() && layer_heatmap_multires_enabled[i]) ? 1ULL : 0ULL);
                hash_combine_f(heatmap_key, i < layer_heatmap_multires_blend.size() ? layer_heatmap_multires_blend[i] : heatmap_multires_blend);
            }
            hash_combine_u64(heatmap_key, (uint64_t)layers[i].features.size());
        }
        if (heatmap_async_inflight && heatmap_async_future.valid() &&
            heatmap_async_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            const auto heat_prof_begin = std::chrono::steady_clock::now();
            auto done = heatmap_async_future.get();
            heatmap_async_inflight = false;
            if (done.first == heatmap_key) {
                heatmap_cached_cells = std::move(done.second.cells);
                if (done.second.has_raster && done.second.raster.w > 0 && done.second.raster.h > 0 && !done.second.raster.rgba.empty()) {
                    heatmap_cached_raster_meta = done.second.raster;
                    heatmap_raster_texture_valid = uploadRgbaTexture(
                        done.second.raster.rgba.data(),
                        (uint32_t)done.second.raster.w,
                        (uint32_t)done.second.raster.h,
                        heatmap_raster_texture);
                    if (heatmap_raster_texture_valid) heatmap_raster_cache_key = done.first;
                } else {
                    if (!smooth_only_heatmap) {
                        destroyTileTexture(heatmap_raster_texture);
                        heatmap_raster_texture_valid = false;
                        heatmap_cached_raster_meta = {};
                        heatmap_raster_cache_key = 0;
                    }
                }
                heatmap_cache_key = done.first;
                heatmap_cache_valid = true;
            }
            prof_heatmap_ms_last.store(prof_ms_since(heat_prof_begin), std::memory_order_relaxed);
        }
        const bool can_use_cached_heatmap = heatmap_cache_valid && heatmap_cache_key == heatmap_key;
        bool should_recompute_heatmap = (!can_use_cached_heatmap) && (!heatmap_controls_active);
        const auto layer_prof_begin = std::chrono::steady_clock::now();
        for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
            auto& l = layers[layer_idx];
            if (!l.enabled) continue;
            const bool project_to_parcels_only =
                (vacant_notice_layer_idx >= 0 && (int)layer_idx == vacant_notice_layer_idx) ||
                (vacant_rehab_layer_idx >= 0 && (int)layer_idx == vacant_rehab_layer_idx) ||
                (tax_lien_layer_idx >= 0 && (int)layer_idx == tax_lien_layer_idx) ||
                (tax_sale_layer_idx >= 0 && (int)layer_idx == tax_sale_layer_idx);
            if (project_to_parcels_only) continue;
            const bool is_zoning_layer = ((int)layer_idx == zoning_layer_idx);
            const bool is_heat_layer = !l.heatmap_field.empty();
            float heat_min = std::numeric_limits<float>::infinity();
            float heat_max = -std::numeric_limits<float>::infinity();
            if (is_heat_layer) {
                for (size_t fi = 0; fi < l.features.size(); ++fi) {
                    const auto& fg = l.features[fi];
                    if (!feature_passes_filters(layer_idx, fi, fg)) continue;
                    float v = 0.0f;
                    if (!try_get_prop_float(fg, l.heatmap_field, v)) continue;
                    heat_min = std::min(heat_min, v);
                    heat_max = std::max(heat_max, v);
                }
            }
            const bool heat_range_valid = std::isfinite(heat_min) && std::isfinite(heat_max) && heat_max > heat_min;
            ImU32 c = ImGui::ColorConvertFloat4ToU32(l.color);
            const bool layer_uses_heatmap_for_cache =
                layer_idx < layer_heatmap_enabled.size() &&
                layer_idx < layer_heatmap_max_zoom.size() &&
                layer_heatmap_enabled[layer_idx] &&
                zoom <= layer_heatmap_max_zoom[layer_idx];
            bool have_candidates = !should_recompute_heatmap || !layer_uses_heatmap_for_cache;
            if (have_candidates) {
                have_candidates = queryLayerSpatialIndex(
                    layer_spatial[layer_idx], view_min_lon, view_min_lat, view_max_lon, view_max_lat, render_candidates);
            }
            if (have_candidates) {
                for (uint32_t fidx : render_candidates) {
                    prof_features_considered_frame++;
                    const bool layer_uses_heatmap =
                        layer_idx < layer_heatmap_enabled.size() &&
                        layer_idx < layer_heatmap_max_zoom.size() &&
                        layer_heatmap_enabled[layer_idx] &&
                        zoom <= layer_heatmap_max_zoom[layer_idx];
                    if (smooth_only_heatmap && should_recompute_heatmap && layer_uses_heatmap && fidx % 2 != 0) continue;
                    if (fidx >= l.features.size()) continue;
                    auto& fg = l.features[(size_t)fidx];
                    if (!feature_passes_filters(layer_idx, (size_t)fidx, fg)) continue;
                    ImU32 feature_c = c;
                    if (is_heat_layer) {
                        float v = 0.0f;
                        if (try_get_prop_float(fg, l.heatmap_field, v) && heat_range_valid) {
                            const float t = (v - heat_min) / (heat_max - heat_min);
                            feature_c = ImGui::ColorConvertFloat4ToU32(heat_color(t));
                        }
                    }
                    if (is_zoning_layer) {
                        const std::string zkey = zoningClassKey(fg);
                        auto it_en = zoning_zone_enabled.find(zkey);
                        if (it_en != zoning_zone_enabled.end() && !it_en->second) continue;
                        auto it_col = zoning_zone_color.find(zkey);
                        if (it_col != zoning_zone_color.end()) feature_c = ImGui::ColorConvertFloat4ToU32(it_col->second);
                    }
                    const auto& ex = fg.extent;
                    const auto& pww = get_world_extent(layer_idx, fidx, fg);
                    ImVec2 p0w = pww.first;
                    ImVec2 p1w = pww.second;
                    ImVec2 a = project_world(p0w);
                    ImVec2 b = project_world(p1w);
                    ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
                    ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
                    if (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y) continue;

                    if (layer_uses_heatmap) {
                        if (can_use_cached_heatmap) continue;
                        const ImVec2 center((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                        HeatSample hs;
                        hs.x = center.x;
                        hs.y = center.y;
                        hs.lon = (fg.extent.min_lon + fg.extent.max_lon) * 0.5f;
                        hs.lat = (fg.extent.min_lat + fg.extent.max_lat) * 0.5f;
                        hs.color = ImGui::ColorConvertU32ToFloat4(feature_c);
                        hs.prefer_gradient = layer_idx < layer_heatmap_use_gradient.size() ? layer_heatmap_use_gradient[layer_idx] : true;
                        resolve_layer_heat_settings(layer_idx, hs);
                        heat_samples.push_back(hs);
                        continue;
                    }

                    if (!fg.rings.empty()) {
                        const auto& world_rings = get_world_rings(layer_idx, fidx, fg);
                        const bool fill_enabled_for_layer = layer_idx < layer_fill_enabled.size() && layer_fill_enabled[layer_idx];
                        if (fill_enabled_for_layer && should_fill_layer_polygon(layer_idx)) {
                            ImU32 fill = (feature_c & 0x00FFFFFF) | (170u << 24);
                            draw_tessellated_fill(fg, world_rings, fill);
                        }

                        for (const auto& r : world_rings) {
                            append_world_ring_line(r);
                            draw->AddPolyline(scratch_line.data(), (int)scratch_line.size(), feature_c, ImDrawFlags_Closed, 1.0f);
                        }
                    } else {
                        ImVec2 pw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, math_zoom);
                        ImVec2 ps = project_world(pw);
                        if (ps.x >= origin.x && ps.x <= origin.x + size.x && ps.y >= origin.y && ps.y <= origin.y + size.y) {
                            float r = std::clamp((float)(2.0 * zoom_scale + 1.5), 2.0f, 6.0f);
                            draw->AddCircleFilled(ps, r, feature_c);
                            prof_features_drawn_frame++;
                        }
                    }

                }
                continue;
            }
            const size_t smooth_sample_stride =
                (smooth_only_heatmap && should_recompute_heatmap && layer_uses_heatmap_for_cache && l.features.size() > kMaxSmoothHeatSamplesPerLayer)
                    ? std::max<size_t>(1, (l.features.size() + kMaxSmoothHeatSamplesPerLayer - 1) / kMaxSmoothHeatSamplesPerLayer)
                    : 1;
            for (auto& fg : l.features) {
                prof_features_considered_frame++;
                size_t fi = (size_t)(&fg - &l.features[0]);
                if (!feature_passes_filters(layer_idx, fi, fg)) continue;
                ImU32 feature_c = c;
                if (is_heat_layer) {
                    float v = 0.0f;
                    if (try_get_prop_float(fg, l.heatmap_field, v) && heat_range_valid) {
                        const float t = (v - heat_min) / (heat_max - heat_min);
                        feature_c = ImGui::ColorConvertFloat4ToU32(heat_color(t));
                    }
                }
                if (is_zoning_layer) {
                    const std::string zkey = zoningClassKey(fg);
                    auto it_en = zoning_zone_enabled.find(zkey);
                    if (it_en != zoning_zone_enabled.end() && !it_en->second) continue;
                    auto it_col = zoning_zone_color.find(zkey);
                    if (it_col != zoning_zone_color.end()) feature_c = ImGui::ColorConvertFloat4ToU32(it_col->second);
                }
                const auto& ex = fg.extent;
                const auto& pww = get_world_extent(layer_idx, (uint32_t)fi, fg);
                ImVec2 p0w = pww.first;
                ImVec2 p1w = pww.second;
                ImVec2 a = project_world(p0w);
                ImVec2 b = project_world(p1w);
                ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
                ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
                const bool layer_uses_heatmap =
                    layer_idx < layer_heatmap_enabled.size() &&
                    layer_idx < layer_heatmap_max_zoom.size() &&
                    layer_heatmap_enabled[layer_idx] &&
                    zoom <= layer_heatmap_max_zoom[layer_idx];
                if (!layer_uses_heatmap && (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y)) continue;
                if (layer_uses_heatmap) {
                    if (can_use_cached_heatmap) continue;
                    if (smooth_sample_stride > 1 && (fi % smooth_sample_stride) != 0) continue;
                    const ImVec2 center((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                    HeatSample hs;
                    hs.x = center.x;
                    hs.y = center.y;
                    hs.lon = (fg.extent.min_lon + fg.extent.max_lon) * 0.5f;
                    hs.lat = (fg.extent.min_lat + fg.extent.max_lat) * 0.5f;
                    hs.color = ImGui::ColorConvertU32ToFloat4(feature_c);
                    hs.prefer_gradient = layer_idx < layer_heatmap_use_gradient.size() ? layer_heatmap_use_gradient[layer_idx] : true;
                    resolve_layer_heat_settings(layer_idx, hs);
                    heat_samples.push_back(hs);
                    continue;
                }

                if (!fg.rings.empty()) {
                    const auto& world_rings = get_world_rings(layer_idx, (uint32_t)fi, fg);
                    const bool fill_enabled_for_layer = layer_idx < layer_fill_enabled.size() && layer_fill_enabled[layer_idx];
                    if (fill_enabled_for_layer && should_fill_layer_polygon(layer_idx)) {
                        ImU32 fill = (feature_c & 0x00FFFFFF) | (170u << 24);
                        draw_tessellated_fill(fg, world_rings, fill);
                    }

                    // Project vacant point datasets onto parcel polygons instead of drawing point markers.
                    if (layer_idx == (size_t)parcel_layer_idx && (vacant_notice_enabled || vacant_rehab_enabled)) {
                        int vac_notice = 0;
                        int vac_rehab = 0;
                        if (fi < parcel_vac_notice_by_feature.size()) vac_notice = parcel_vac_notice_by_feature[fi];
                        if (fi < parcel_vac_rehab_by_feature.size()) vac_rehab = parcel_vac_rehab_by_feature[fi];

                        int weight = 0;
                        if (vacant_notice_enabled) weight += vac_notice;
                        if (vacant_rehab_enabled) weight += vac_rehab;
                        if (weight > 0) {
                            const bool notice_fill = vacant_notice_enabled &&
                                vacant_notice_layer_idx >= 0 &&
                                (size_t)vacant_notice_layer_idx < layer_fill_enabled.size() &&
                                layer_fill_enabled[(size_t)vacant_notice_layer_idx];
                            const bool rehab_fill = vacant_rehab_enabled &&
                                vacant_rehab_layer_idx >= 0 &&
                                (size_t)vacant_rehab_layer_idx < layer_fill_enabled.size() &&
                                layer_fill_enabled[(size_t)vacant_rehab_layer_idx];
                            if ((notice_fill || rehab_fill) && should_fill_layer_polygon((size_t)parcel_layer_idx)) {
                                const int alpha = std::clamp(95 + weight * 16, 95, 220);
                                ImU32 vac_fill = color_with_alpha(vacancy_base_color(vac_notice, vac_rehab), alpha);
                                draw_tessellated_fill(fg, world_rings, vac_fill);
                            }
                        }
                    }

                    for (const auto& r : world_rings) {
                        append_world_ring_line(r);
                        draw->AddPolyline(scratch_line.data(), (int)scratch_line.size(), feature_c, ImDrawFlags_Closed, 1.0f);
                    }
                } else {
                    ImVec2 pw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, math_zoom);
                    ImVec2 ps = project_world(pw);
                    if (ps.x >= origin.x && ps.x <= origin.x + size.x && ps.y >= origin.y && ps.y <= origin.y + size.y) {
                        float r = std::clamp((float)(2.0 * zoom_scale + 1.5), 2.0f, 6.0f);
                        draw->AddCircleFilled(ps, r, feature_c);
                        prof_features_drawn_frame++;
                    }
                }

            }
        }
        prof_layer_ms_last.store(prof_ms_since(layer_prof_begin), std::memory_order_relaxed);
        prof_heat_samples_last.store(heat_samples.size(), std::memory_order_relaxed);
        if (should_recompute_heatmap && !heatmap_async_inflight) {
            const auto heat_prof_begin = std::chrono::steady_clock::now();
            const auto samples_copy = heat_samples;
            const uint64_t key_copy = heatmap_key;
            const float cell_copy = global_heat_cell;
            const float bw_copy = heatmap_bandwidth_px;
            const float blur_copy = heatmap_blur_sigma_px;
            const float clip_copy = heatmap_percentile_clip;
            const bool adaptive_copy = heatmap_zoom_adaptive_bandwidth;
            const bool multires_enabled_copy = heatmap_multires_enabled;
            const float multires_blend_copy = heatmap_multires_blend;
            const int zoom_copy = zoom;
            const float ox = origin.x;
            const float oy = origin.y;
            const float vw = std::max(1.0f, size.x);
            const float vh = std::max(1.0f, size.y);
            heatmap_async_future = std::async(std::launch::async, [=]() mutable -> std::pair<uint64_t, HeatmapRenderData> {
                return buildHeatmapRenderData(
                    key_copy,
                    samples_copy,
                    ox,
                    oy,
                    view_min_lon,
                    view_min_lat,
                    view_max_lon,
                    view_max_lat,
                    vw,
                    vh,
                    zoom_copy,
                    kMaxZoom,
                    kSmoothHeatRasterBasePx,
                    kSmoothHeatRasterMaxPx);
            });
            heatmap_async_inflight = true;
            if (smooth_only_heatmap) should_recompute_heatmap = false;
        } else if (heatmap_async_inflight) {
            should_recompute_heatmap = false;
        }
        std::vector<CachedHeatCell> frame_heat_cells;
        if (should_recompute_heatmap) {
            frame_heat_cells = buildImmediateHeatmapCells(
                heat_samples,
                origin.x,
                origin.y,
                size.x,
                size.y,
                zoom,
                kMaxZoom,
                heatmap_algo,
                global_heat_cell,
                heatmap_bandwidth_px,
                heatmap_blur_sigma_px,
                heatmap_percentile_clip,
                heatmap_zoom_adaptive_bandwidth,
                heatmap_multires_enabled,
                heatmap_multires_blend);
            heatmap_cached_cells = frame_heat_cells;
            heatmap_cache_key = heatmap_key;
            heatmap_cache_valid = true;
        }
        const std::vector<CachedHeatCell>& draw_cells =
            can_use_cached_heatmap ? heatmap_cached_cells :
            (should_recompute_heatmap ? frame_heat_cells : heatmap_cached_cells);
        const bool draw_stale_raster_while_rebuilding =
            smooth_only_heatmap &&
            heatmap_async_inflight &&
            heatmap_raster_texture_valid;
        const bool draw_current_raster =
            heatmap_raster_texture_valid &&
            heatmap_raster_cache_key == heatmap_key;
        const bool drawing_heatmap_raster =
            (draw_current_raster || draw_stale_raster_while_rebuilding) &&
            heatmap_raster_texture.descriptor;
        if (drawing_heatmap_raster) {
            ImVec2 raster_nw = lonLatToWorldPx(heatmap_cached_raster_meta.min_lon, heatmap_cached_raster_meta.max_lat, math_zoom);
            ImVec2 raster_se = lonLatToWorldPx(heatmap_cached_raster_meta.max_lon, heatmap_cached_raster_meta.min_lat, math_zoom);
            ImVec2 rp0 = project_world(raster_nw);
            ImVec2 rp1 = project_world(raster_se);
            draw->AddImage((ImTextureID)heatmap_raster_texture.descriptor, rp0, rp1);
        }
        const bool suppress_vector_heat_cells = smooth_only_heatmap && drawing_heatmap_raster;
        if (!suppress_vector_heat_cells) for (const auto& c : draw_cells) {
            if (c.is_hex) {
                ImVec2 pts[6] = {
                    ImVec2(c.cx - c.hw, c.cy),
                    ImVec2(c.cx - c.hw * 0.5f, c.cy - c.hh),
                    ImVec2(c.cx + c.hw * 0.5f, c.cy - c.hh),
                    ImVec2(c.cx + c.hw, c.cy),
                    ImVec2(c.cx + c.hw * 0.5f, c.cy + c.hh),
                    ImVec2(c.cx - c.hw * 0.5f, c.cy + c.hh),
                };
                draw->AddConvexPolyFilled(pts, 6, c.fill);
                if (c.draw_outline) draw->AddPolyline(pts, 6, c.outline, ImDrawFlags_Closed, 1.0f);
            } else {
                draw->AddRectFilled(ImVec2(c.x0, c.y0), ImVec2(c.x1, c.y1), c.fill);
                if (c.draw_outline) draw->AddRect(ImVec2(c.x0, c.y0), ImVec2(c.x1, c.y1), c.outline, 0.0f, 0, 1.0f);
            }
        }

        const auto overlay_prof_begin = std::chrono::steady_clock::now();
        // Render vacant overlays as a final top pass so they remain visible regardless of layer order.
        size_t visible_vacant_parcels_counter = 0;
        if ((vacant_notice_enabled || vacant_rehab_enabled) && parcel_layer_idx >= 0) {
            auto& parcel_layer = layers[(size_t)parcel_layer_idx];
            for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
                auto& fg = parcel_layer.features[i];
                if (!feature_passes_filters((size_t)parcel_layer_idx, i, fg)) continue;
                const auto& ex = fg.extent;
                const auto& pww = get_world_extent((size_t)parcel_layer_idx, (uint32_t)i, fg);
                ImVec2 p0w = pww.first;
                ImVec2 p1w = pww.second;
                ImVec2 a = project_world(p0w);
                ImVec2 b = project_world(p1w);
                ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
                ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
                if (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y) continue;
                if (fg.rings.empty()) continue;

                int vac_notice = (i < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[i] : 0;
                int vac_rehab = (i < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[i] : 0;

                int weight = 0;
                if (vacant_notice_enabled) weight += vac_notice;
                if (vacant_rehab_enabled) weight += vac_rehab;
                if (weight <= 0) continue;
                visible_vacant_parcels_counter++;

                const auto& world_rings = get_world_rings((size_t)parcel_layer_idx, (uint32_t)i, fg);
                const int alpha = std::clamp(120 + weight * 18, 120, 230);
                ImVec4 vac_base = vacancy_base_color(vac_notice, vac_rehab);
                ImU32 vac_fill = color_with_alpha(vac_base, alpha);
                ImU32 vac_outline = color_with_alpha(darken_color(vac_base, 0.62f), 235);
                const bool notice_fill = vacant_notice_layer_idx >= 0 &&
                    (size_t)vacant_notice_layer_idx < layer_fill_enabled.size() &&
                    layer_fill_enabled[(size_t)vacant_notice_layer_idx];
                const bool rehab_fill = vacant_rehab_layer_idx >= 0 &&
                    (size_t)vacant_rehab_layer_idx < layer_fill_enabled.size() &&
                    layer_fill_enabled[(size_t)vacant_rehab_layer_idx];
                if ((notice_fill || rehab_fill) && should_fill_layer_polygon((size_t)parcel_layer_idx)) {
                    draw_tessellated_fill(fg, world_rings, vac_fill);
                }
                for (const auto& r : world_rings) {
                    append_world_ring_line(r);
                    if (!scratch_line.empty()) draw->AddPolyline(scratch_line.data(), (int)scratch_line.size(), vac_outline, ImDrawFlags_Closed, 2.0f);
                }
            }
        }
        visible_vacant_parcels_last_frame.store(visible_vacant_parcels_counter, std::memory_order_relaxed);
        prof_overlay_ms_last.store(prof_ms_since(overlay_prof_begin), std::memory_order_relaxed);

        // Render tax source records on matched parcel geometry, not as raw point dots.
        if ((tax_lien_enabled || tax_sale_enabled) && parcel_layer_idx >= 0) {
            auto& parcel_layer = layers[(size_t)parcel_layer_idx];
            for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
                auto& fg = parcel_layer.features[i];
                if (!feature_passes_filters((size_t)parcel_layer_idx, i, fg)) continue;
                const auto& pww = get_world_extent((size_t)parcel_layer_idx, (uint32_t)i, fg);
                ImVec2 a = project_world(pww.first);
                ImVec2 b = project_world(pww.second);
                ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
                ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
                if (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y) continue;
                if (fg.rings.empty()) continue;

                const int lien_count = (i < parcel_tax_lien_by_feature.size()) ? parcel_tax_lien_by_feature[i] : 0;
                const int sale_count = (i < parcel_tax_sale_by_feature.size()) ? parcel_tax_sale_by_feature[i] : 0;
                int weight = 0;
                if (tax_lien_enabled) weight += lien_count;
                if (tax_sale_enabled) weight += sale_count;
                if (weight <= 0) continue;

                ImVec4 lien_c = (tax_lien_layer_idx >= 0) ? layers[(size_t)tax_lien_layer_idx].color : ImVec4(0.95f, 0.55f, 0.1f, 1.0f);
                ImVec4 sale_c = (tax_sale_layer_idx >= 0) ? layers[(size_t)tax_sale_layer_idx].color : ImVec4(0.85f, 0.2f, 0.1f, 1.0f);
                ImVec4 tax_base = lien_c;
                if (tax_lien_enabled && tax_sale_enabled && lien_count > 0 && sale_count > 0) {
                    tax_base = ImVec4((lien_c.x + sale_c.x) * 0.5f,
                                      (lien_c.y + sale_c.y) * 0.5f,
                                      (lien_c.z + sale_c.z) * 0.5f,
                                      1.0f);
                } else if (tax_sale_enabled && sale_count > 0) {
                    tax_base = sale_c;
                }
                const auto& world_rings = get_world_rings((size_t)parcel_layer_idx, (uint32_t)i, fg);
                const bool lien_fill = tax_lien_enabled &&
                    tax_lien_layer_idx >= 0 &&
                    (size_t)tax_lien_layer_idx < layer_fill_enabled.size() &&
                    layer_fill_enabled[(size_t)tax_lien_layer_idx];
                const bool sale_fill = tax_sale_enabled &&
                    tax_sale_layer_idx >= 0 &&
                    (size_t)tax_sale_layer_idx < layer_fill_enabled.size() &&
                    layer_fill_enabled[(size_t)tax_sale_layer_idx];
                if ((lien_fill || sale_fill) && should_fill_layer_polygon((size_t)parcel_layer_idx)) {
                    const int alpha = std::clamp(90 + weight * 10, 90, 210);
                    draw_tessellated_fill(fg, world_rings, color_with_alpha(tax_base, alpha));
                }
                ImU32 tax_outline = color_with_alpha(darken_color(tax_base, 0.58f), 240);
                for (const auto& r : world_rings) {
                    append_world_ring_line(r);
                    if (!scratch_line.empty()) draw->AddPolyline(scratch_line.data(), (int)scratch_line.size(), tax_outline, ImDrawFlags_Closed, 2.0f);
                }
            }
        }

        render_fill_attempts_last_frame.store(fill_attempts_frame, std::memory_order_relaxed);
        render_fill_success_last_frame.store(fill_success_frame, std::memory_order_relaxed);
        render_fill_no_triangles_last_frame.store(fill_no_triangles_frame, std::memory_order_relaxed);
        render_fill_bad_indices_last_frame.store(fill_bad_indices_frame, std::memory_order_relaxed);

        {
            char zoom_label[32];
            std::snprintf(zoom_label, sizeof(zoom_label), "Zoom %d", zoom);
            const ImVec2 text_size = ImGui::CalcTextSize(zoom_label);
            const float pad_x = 10.0f;
            const float pad_y = 6.0f;
            const ImVec2 box_max(origin.x + size.x - 12.0f, origin.y + size.y - 12.0f);
            const ImVec2 box_min(box_max.x - text_size.x - pad_x * 2.0f, box_max.y - text_size.y - pad_y * 2.0f);
            draw->AddRectFilled(box_min, box_max, IM_COL32(17, 24, 32, 205), 7.0f);
            draw->AddRect(box_min, box_max, IM_COL32(255, 255, 255, 70), 7.0f);
            draw->AddText(ImVec2(box_min.x + pad_x, box_min.y + pad_y), IM_COL32(245, 248, 250, 245), zoom_label);
        }

        if (map_hovered && parcel_inspect_active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered_parcel != nullptr) {
            show_selected_parcel_details = true;
            selected_parcel_idx = hovered_parcel_idx;
            show_selected_zone_details = false;
            selected_zone_idx = (size_t)-1;
        } else if (map_hovered && zoning_inspect_active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered_zone != nullptr) {
            show_selected_zone_details = true;
            selected_zone_idx = hovered_zone_idx;
            show_selected_parcel_details = false;
            selected_parcel_idx = (size_t)-1;
        }

        if (parcel_hover_active && map_hovered && hovered_parcel) {
            std::string blocklot_raw = getPropertyValue(*hovered_parcel, "BLOCKLOT");
            std::string blocklot = normalizeJoinKey(blocklot_raw);
            int vac_notice = (hovered_parcel_idx < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[hovered_parcel_idx] : 0;
            int vac_rehab = (hovered_parcel_idx < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[hovered_parcel_idx] : 0;
            int tax_lien = (hovered_parcel_idx < parcel_tax_lien_by_feature.size()) ? parcel_tax_lien_by_feature[hovered_parcel_idx] : 0;
            int tax_sale = (hovered_parcel_idx < parcel_tax_sale_by_feature.size()) ? parcel_tax_sale_by_feature[hovered_parcel_idx] : 0;
            double tax_lien_amount = (hovered_parcel_idx < parcel_tax_lien_amount_by_feature.size()) ? parcel_tax_lien_amount_by_feature[hovered_parcel_idx] : 0.0;
            double tax_sale_amount = (hovered_parcel_idx < parcel_tax_sale_amount_by_feature.size()) ? parcel_tax_sale_amount_by_feature[hovered_parcel_idx] : 0.0;
            const LayerDef::FeatureGeom* hovered_zoning = hovered_zone;
            if (zoning_layer_idx >= 0 && (size_t)zoning_layer_idx < layers.size()) {
                const float qlon = (hovered_parcel->extent.min_lon + hovered_parcel->extent.max_lon) * 0.5f;
                const float qlat = (hovered_parcel->extent.min_lat + hovered_parcel->extent.max_lat) * 0.5f;
                std::vector<uint32_t> zoning_candidates;
                bool have_zoning_candidates = false;
                if ((size_t)zoning_layer_idx < layer_spatial.size() && layer_spatial[(size_t)zoning_layer_idx].built) {
                    have_zoning_candidates = queryLayerSpatialIndex(
                        layer_spatial[(size_t)zoning_layer_idx], qlon, qlat, qlon, qlat, zoning_candidates);
                }
                if (have_zoning_candidates) {
                    const auto& zfeats = layers[(size_t)zoning_layer_idx].features;
                    for (uint32_t zi : zoning_candidates) {
                        if (zi >= zfeats.size()) continue;
                        const auto& zf = zfeats[zi];
                        if (qlon >= zf.extent.min_lon && qlon <= zf.extent.max_lon &&
                            qlat >= zf.extent.min_lat && qlat <= zf.extent.max_lat &&
                            pointInFeature(zf, qlon, qlat)) {
                            hovered_zoning = &zf;
                            break;
                        }
                    }
                }
            }

            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Parcel Details");
            ImGui::Separator();
            ImGui::Text("BLOCKLOT: %s", blocklot_raw.empty() ? "(none)" : blocklot_raw.c_str());
            ImGui::Text("Vacant Notices: %d", vac_notice);
            ImGui::Text("Vacant Rehab Records: %d", vac_rehab);
            ImGui::Text("Tax Lien Certificate Records: %d", tax_lien);
            if (tax_lien > 0) ImGui::Text("Tax Lien Total Amount: $%.2f", tax_lien_amount);
            ImGui::Text("Tax Sale 2021 Records: %d", tax_sale);
            if (tax_sale > 0) ImGui::Text("Tax Sale Total Lien: $%.2f", tax_sale_amount);

            const LayerDef::FeatureGeom* hovered_rp = real_property_for_parcel(*hovered_parcel);
            draw_real_property_summary(hovered_rp);

            ImGui::Separator();
            if (hovered_zoning) {
                std::string zone_key = zoningClassKey(*hovered_zoning);
                std::string zone_label = zoningClassLabel(*hovered_zoning);
                std::string zone_description;
                auto meta_it = zoning_metadata.find(zone_key);
                if (meta_it != zoning_metadata.end()) {
                    if (!meta_it->second.label.empty()) zone_label = meta_it->second.label;
                    zone_description = meta_it->second.description;
                }
                ImGui::Text("Zoning: %s", zone_key.empty() ? "(available, unlabeled)" : zone_key.c_str());
                if (!zone_label.empty() && zone_label != zone_key) {
                    ImGui::TextWrapped("Zoning Label: %s", zone_label.c_str());
                }
                if (!zone_description.empty()) {
                    ImGui::TextWrapped("Description: %s", zone_description.c_str());
                }
                ImGui::TextUnformatted("Zoning Fields");
                for (const auto& kv : hovered_zoning->properties) {
                    if (kv.second.empty()) continue;
                    ImGui::TextWrapped("%s: %s", kv.first.c_str(), kv.second.c_str());
                }
            } else if (zoning_layer_idx >= 0) {
                ImGui::TextDisabled("Zoning: no intersecting zoning polygon found.");
            }
            ImGui::Separator();
            draw_feature_properties("All Parcel Geometry Fields", *hovered_parcel);
            if (hovered_rp) {
                ImGui::Separator();
                draw_feature_properties("All Real Property Fields", *hovered_rp);
            }
            ImGui::EndTooltip();
        }
        if (zoning_hover_active && map_hovered && !hovered_parcel && hovered_zone) {
            std::string zone_key = zoningClassKey(*hovered_zone);
            std::string zone_label = zoningClassLabel(*hovered_zone);
            std::string zone_description;
            auto meta_it = zoning_metadata.find(zone_key);
            if (meta_it != zoning_metadata.end()) {
                if (!meta_it->second.label.empty()) zone_label = meta_it->second.label;
                zone_description = meta_it->second.description;
            }
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Zoning Details");
            ImGui::Separator();
            ImGui::Text("Feature: %zu", hovered_zone_idx);
            ImGui::Text("Zone: %s", zone_key.empty() ? "(unlabeled)" : zone_key.c_str());
            if (!zone_label.empty() && zone_label != zone_key) {
                ImGui::TextWrapped("Label: %s", zone_label.c_str());
            }
            if (!zone_description.empty()) {
                ImGui::TextWrapped("Description: %s", zone_description.c_str());
            }
            ImGui::Separator();
            ImGui::TextUnformatted("All Zone Fields");
            for (const auto& kv : hovered_zone->properties) {
                if (kv.second.empty()) continue;
                ImGui::TextWrapped("%s: %s", kv.first.c_str(), kv.second.c_str());
            }
            ImGui::EndTooltip();
        }
        ImGui::EndTabItem();
        }
        TimeCubePanelContext time_cube_panel_ctx;
        time_cube_panel_ctx.service = &time_cube_service;
        time_cube_panel_ctx.layers = &layers;
        time_cube_panel_ctx.result = &time_cube_ui_result;
        time_cube_panel_ctx.loaded = &time_cube_ui_loaded;
        time_cube_panel_ctx.status = &time_cube_ui_status;
        time_cube_panel_ctx.mutex = &time_cube_ui_mutex;
        time_cube_panel_ctx.worker = &time_cube_ui_worker;
        time_cube_panel_ctx.running = &time_cube_ui_running;
        time_cube_panel_ctx.done = &time_cube_ui_done;
        time_cube_panel_ctx.selected = &time_cube_selected;
        time_cube_panel_ctx.year_min = &time_cube_year_min;
        time_cube_panel_ctx.year_max = &time_cube_year_max;
        time_cube_panel_ctx.normalize_mode = &time_cube_normalize_mode;
        time_cube_panel_ctx.show_excluded = &time_cube_show_excluded;
        drawTimeCubeTab(time_cube_panel_ctx);
        PolicyPanelContext policy_panel_ctx;
        policy_panel_ctx.hierarchy = &policy_hierarchy;
        policy_panel_ctx.hierarchy_loaded = policy_hierarchy_loaded;
        policy_panel_ctx.hierarchy_error = &policy_hierarchy_error;
        policy_panel_ctx.query = policy_hierarchy_query;
        policy_panel_ctx.query_capacity = sizeof(policy_hierarchy_query);
        policy_panel_ctx.scope = &policy_hierarchy_scope;
        policy_panel_ctx.roster = &public_servant_roster;
        policy_panel_ctx.people_pay_cached_query = &people_pay_cached_query;
        policy_panel_ctx.people_pay_cached_scope = &people_pay_cached_scope;
        policy_panel_ctx.people_pay_cache_matched_count = &people_pay_cache_matched_count;
        policy_panel_ctx.people_pay_visible_rows = &people_pay_visible_rows;
        policy_panel_ctx.people_pay_cache_rebuilds = &people_pay_cache_rebuilds;
        policy_panel_ctx.people_pay_rendered_rows_last = &people_pay_rendered_rows_last;
        policy_panel_ctx.viz_root = &policy_viz_root;
        policy_panel_ctx.viz_cached_query = &policy_viz_cached_query;
        policy_panel_ctx.viz_cached_scope = &policy_viz_cached_scope;
        policy_panel_ctx.viz_cached_metric = &policy_viz_cached_metric;
        policy_panel_ctx.viz_metric = &policy_viz_metric;
        policy_panel_ctx.viz_cache_rebuilds = &policy_viz_cache_rebuilds;
        policy_panel_ctx.viz_node_count = &policy_viz_node_count;
        drawPolicyHierarchyTab(policy_panel_ctx);
        drawVisualModelTabs(layers.size());
        ImGui::EndTabBar();
        }

        ImGui::End();

        FrameFinalizationContext frame_finalization_ctx;
        frame_finalization_ctx.window_data = wd;
        frame_finalization_ctx.last_frame_ts = &last_frame_ts;
        frame_finalization_ctx.ema_frame_ms = &ema_frame_ms;
        frame_finalization_ctx.perf_alpha = kPerfAlpha;
        frame_finalization_ctx.frame_begin = prof_frame_begin;
        frame_finalization_ctx.tiles_drawn_frame = prof_tiles_drawn_frame;
        frame_finalization_ctx.features_considered_frame = prof_features_considered_frame;
        frame_finalization_ctx.features_drawn_frame = prof_features_drawn_frame;
        frame_finalization_ctx.perf_frame_ms_last = &perf_frame_ms_last;
        frame_finalization_ctx.perf_frame_ms_avg = &perf_frame_ms_avg;
        frame_finalization_ctx.perf_fps_avg = &perf_fps_avg;
        frame_finalization_ctx.prof_ui_ms_last = &prof_ui_ms_last;
        frame_finalization_ctx.prof_owner_ms_last = &prof_owner_ms_last;
        frame_finalization_ctx.prof_tile_ms_last = &prof_tile_ms_last;
        frame_finalization_ctx.prof_layer_ms_last = &prof_layer_ms_last;
        frame_finalization_ctx.prof_heatmap_ms_last = &prof_heatmap_ms_last;
        frame_finalization_ctx.prof_overlay_ms_last = &prof_overlay_ms_last;
        frame_finalization_ctx.prof_present_ms_last = &prof_present_ms_last;
        frame_finalization_ctx.prof_tiles_drawn_last = &prof_tiles_drawn_last;
        frame_finalization_ctx.prof_features_considered_last = &prof_features_considered_last;
        frame_finalization_ctx.prof_features_drawn_last = &prof_features_drawn_last;
        frame_finalization_ctx.prof_retired_textures = &prof_retired_textures;
        frame_finalization_ctx.prof_tile_cache_size = &prof_tile_cache_size;
        frame_finalization_ctx.prof_heat_samples_last = &prof_heat_samples_last;
        frame_finalization_ctx.profile_mutex = &profile_mutex;
        frame_finalization_ctx.profile_samples = &profile_samples;
        frame_finalization_ctx.profile_sample_pos = &profile_sample_pos;
        frame_finalization_ctx.profile_sample_count = &profile_sample_count;
        finalizeWorldSimFrame(frame_finalization_ctx);
    }

    AppShutdownContext shutdown_ctx;
    shutdown_ctx.root = &root;
    shutdown_ctx.app_settings = &app_settings;
    shutdown_ctx.window = window;
    shutdown_ctx.layers = &layers;
    shutdown_ctx.hover_inspector_enabled = hover_inspector_enabled;
    shutdown_ctx.zoning_zone_enabled = &zoning_zone_enabled;
    shutdown_ctx.layer_fill_enabled = &layer_fill_enabled;
    shutdown_ctx.layer_hover_enabled = &layer_hover_enabled;
    shutdown_ctx.layer_inspect_enabled = &layer_inspect_enabled;
    shutdown_ctx.layer_heatmap_enabled = &layer_heatmap_enabled;
    shutdown_ctx.layer_heatmap_max_zoom = &layer_heatmap_max_zoom;
    shutdown_ctx.layer_heatmap_use_gradient = &layer_heatmap_use_gradient;
    shutdown_ctx.layer_heatmap_algo = &layer_heatmap_algo;
    shutdown_ctx.layer_heatmap_use_global_settings = &layer_heatmap_use_global_settings;
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
