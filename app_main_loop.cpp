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
#include "choropleth_histogram.h"
#include "color_editor_ipc.h"
#include "color_editor_window.h"
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
#include "render_routing.h"
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
#include "event_sectors.h"
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
#include "owner_text_filter.h"
#include "address_text_filter.h"
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
#include "env_config.h"
#include "app_runtime_bootstrap.h"
#include "app_background_services.h"
#include "app_ui_state_bootstrap.h"
#include "app_window_service.h"
#include "crime_point_runtime_service.h"
#include "parcel_runtime_service.h"
#include "point_layer_runtime_service.h"
#include "polyline_layer_runtime_service.h"
#include "polygon_artifact_runtime_service.h"
#include "zoning_runtime_service.h"
#include "startup_preprocess_service.h"
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
#include "ui_fonts.h"
#include "ui_theme.h"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
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
#include <signal.h>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
struct SingleInstanceLock {
    int fd = -1;
    fs::path path;

    bool acquire(const fs::path& lock_path, std::string& message) {
        path = lock_path;
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd < 0) {
            message = "failed to open " + path.string() + ": " + std::strerror(errno);
            return false;
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            message = "another worldsim3 GUI instance is already running; lock file " + path.string() +
                      " is held: " + std::strerror(errno);
            ::close(fd);
            fd = -1;
            return false;
        }
        const std::string pid_text = std::to_string((long long)::getpid()) + "\n";
        if (::ftruncate(fd, 0) != 0 || ::lseek(fd, 0, SEEK_SET) < 0) {
            message = "failed to prepare " + path.string() + ": " + std::strerror(errno);
            return false;
        }
        const ssize_t written = ::write(fd, pid_text.c_str(), pid_text.size());
        if (written < 0 || static_cast<size_t>(written) != pid_text.size()) {
            message = "failed to write " + path.string() + ": " + std::strerror(errno);
            return false;
        }
        return true;
    }

    ~SingleInstanceLock() {
        if (fd >= 0) {
            (void)::flock(fd, LOCK_UN);
            (void)::close(fd);
        }
    }
};

ImU32 mapPolygonFillColor(ImU32 color, float opacity) {
    const uint32_t src_alpha = (color >> 24) & 0xFFu;
    if (src_alpha == 0) return color;
    const uint32_t alpha = (uint32_t)std::clamp(
        (int)std::lround((float)src_alpha * std::clamp(opacity, 0.0f, 1.0f)),
        0,
        255);
    return (color & 0x00FFFFFFu) | (alpha << 24);
}

}

int runWorldSim3App(int argc, char** argv) {

    fs::path root = resolveAppRoot(fs::current_path(), argc > 0 ? argv[0] : nullptr);
    WorldSimRunState runtime;
    runtime.argc = argc;
    runtime.argv = argv;
    runtime.root = root;
    applyDotEnvEnvironment(root);
    const WorldsimCliOptions cli_options = parseWorldsimCliOptions(argc, argv);
    if (cli_options.run_color_editor) {
        if (cli_options.color_editor_session_file.empty()) return 1;
        return runColorEditorWindow(root, cli_options.color_editor_session_file);
    }
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

    if (cli_options.run_startup_preprocess) {
        StartupPreprocessPlan preprocess_plan = inspectStartupPreprocessPlan(root);
        return runStartupPreprocessCli(
            root,
            app_settings,
            preprocess_plan,
            cli_options,
            argc > 0 ? argv[0] : nullptr);
    }

    SingleInstanceLock gui_instance_lock;
    std::string gui_instance_lock_error;
    if (!gui_instance_lock.acquire(root / "data" / "worldsim3.app.lock", gui_instance_lock_error)) {
        std::cerr << "[worldsim3] " << gui_instance_lock_error << "\n";
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    preloadLayersFromEnvironment(root);

    StartupPreprocessPlan preprocess_plan = inspectStartupPreprocessPlan(root);
    printStartupPreprocessPlan(preprocess_plan, std::cerr);
    if (preprocess_plan.required) {
        const int preprocess_rc = runStartupPreprocessWindow(
            root,
            app_settings,
            preprocess_plan,
            cli_options,
            argc > 0 ? argv[0] : nullptr);
        if (preprocess_rc != 0) {
            std::cerr << "[worldsim3] main UI blocked because required startup preprocessing failed\n";
            return preprocess_rc;
        }
        preprocess_plan = inspectStartupPreprocessPlan(root);
        printStartupPreprocessPlan(preprocess_plan, std::cerr);
        if (preprocess_plan.required) {
            std::cerr << "[worldsim3] main UI blocked because required artifacts are still missing or stale after preprocessing\n";
            return 1;
        }
    }

    g_EnableValidationLayers = app_settings.vulkan_validation_enabled;
    AppWindowServiceContext window_ctx;
    std::string window_error;
    if (!initializeAppWindows(app_settings, window_ctx, &window_error)) {
        if (!window_error.empty()) std::fprintf(stderr, "[worldsim3] window/bootstrap init failed: %s\n", window_error.c_str());
        return 1;
    }
    const int initial_window_w = window_ctx.initial_window_w;
    const int initial_window_h = window_ctx.initial_window_h;
    GLFWwindow* window = window_ctx.main_window;
    int w = window_ctx.main_framebuffer_w;
    int h = window_ctx.main_framebuffer_h;
    ImGui_ImplVulkanH_Window* wd = window_ctx.main_window_data;
    ImGuiContext* main_imgui_context = window_ctx.main_imgui_context;
    GLFWwindow* download_queue_window = window_ctx.download_queue_window;
    ImGui_ImplVulkanH_Window download_queue_wd = window_ctx.download_queue_window_data;
    int dq_w = window_ctx.download_queue_framebuffer_w;
    int dq_h = window_ctx.download_queue_framebuffer_h;
    bool download_queue_swapchain_rebuild = window_ctx.download_queue_swapchain_rebuild;
    ImGuiContext* download_queue_imgui_context = window_ctx.download_queue_imgui_context;

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
    int zoning_layer_idx = layer_indices.zoning_layer_idx;
    const int crime_nibrs_layer_idx = layer_indices.crime_nibrs_layer_idx;
    if (parcel_layer_idx >= 0 && static_cast<size_t>(parcel_layer_idx) < layers.size()) {
        const LayerDef& parcel_layer = layers[static_cast<size_t>(parcel_layer_idx)];
        const std::filesystem::path stored_path = resolveStoredLayerPath(root, parcel_layer);
        const std::filesystem::path canonical_path = canonicalLayerPathForFile(root, parcel_layer.file);
        const LayerRenderRoute render_route = classifyLayerRenderRoute(
            static_cast<size_t>(parcel_layer_idx),
            parcel_layer,
            parcel_layer_idx);
        const std::filesystem::path artifact_path =
            geometryArtifactCachePathForLayerFile(
                root,
                parcel_layer.file,
                GeometryArtifactClass::Polygon,
                layerRenderRouteArtifactName(render_route));
        std::fprintf(
            stderr,
            "[worldsim3] parcel semantic index idx=%d file=%s county=%s renderer=%s stored=%s canonical=%s artifact=%s\n",
            parcel_layer_idx,
            parcel_layer.file.c_str(),
            parcel_layer.provenance_county_city.c_str(),
            layerRenderRouteName(render_route, true),
            stored_path.string().c_str(),
            canonical_path.string().c_str(),
            artifact_path.string().c_str());
    } else {
        std::fprintf(stderr, "[worldsim3] parcel semantic index unavailable: no parcel layer selected\n");
    }
    std::unordered_map<std::string, size_t> real_property_by_blocklot;
    std::vector<LayerDef::FeatureRecord> harmonized_real_property_features;
    std::vector<std::string> harmonized_real_property_source_files;
    std::string harmonized_real_property_signature;
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
    std::vector<std::string> parcel_blocklot_by_feature;
    std::string parcel_blocklot_cached_signature;
    int parcel_parameter_mode = 0;
    std::vector<UnifiedParcelRecord> unified_parcels;
    std::vector<std::string> parcel_owner_search_by_feature;
    std::vector<std::string> real_property_owner_search_by_feature;
    std::vector<std::string> parcel_address_search_by_feature;
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
    std::string cached_vac_notice_signature;
    size_t cached_vac_rehab_size = 0;
    std::string cached_vac_rehab_signature;
    size_t cached_tax_lien_size = 0;
    std::string cached_tax_lien_signature;
    size_t cached_tax_sale_size = 0;
    std::string cached_tax_sale_signature;
    std::string unified_parcel_cached_signature;
    OwnerTextFilterState owner_text_filter_state;
    AddressTextFilterState address_text_filter_state;
    std::string derived_layer_refresh_inputs_signature;
    std::mutex hydrated_mutex;
    std::deque<HydratedLayer> hydrated_queue;
    std::mutex hydrate_req_mutex;
    std::condition_variable hydrate_req_cv;
    std::deque<size_t> hydrate_requests;
    std::mutex spatial_mutex;
    std::condition_variable spatial_cv;
    std::deque<SpatialIndexJob> spatial_jobs;
    std::deque<SpatialIndexResult> spatial_results;
    std::atomic<bool> hydration_stop{false};
    std::atomic<size_t> hydrated_count{0};
    std::atomic<double> perf_frame_ms_avg{0.0};
    std::atomic<double> perf_frame_ms_last{0.0};
    std::atomic<double> perf_fps_avg{0.0};
    std::atomic<double> ui_left_panel_frac{0.34};
    std::atomic<double> ui_right_panel_frac{0.24};
    std::atomic<double> prof_ui_ms_last{0.0};
    std::atomic<double> prof_owner_ms_last{0.0};
    std::atomic<double> prof_tile_ms_last{0.0};
    std::atomic<double> prof_layer_ms_last{0.0};
    std::atomic<double> prof_owner_filter_ms_last{0.0};
    std::atomic<double> prof_heatmap_ms_last{0.0};
    std::atomic<double> prof_overlay_ms_last{0.0};
    std::atomic<double> prof_present_ms_last{0.0};
    std::atomic<size_t> prof_tiles_drawn_last{0};
    std::atomic<size_t> prof_features_considered_last{0};
    std::atomic<size_t> prof_features_drawn_last{0};
    std::atomic<size_t> prof_owner_filter_candidates_last{0};
    std::atomic<size_t> prof_owner_filter_matches_last{0};
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
    std::vector<LayerProfileAccumulator> layer_profile_accumulators(layers.size());
    std::vector<bool> layer_profile_dirty(layers.size(), true);
    std::atomic<size_t> render_fill_attempts_last_frame{0};
    std::atomic<size_t> render_fill_success_last_frame{0};
    std::atomic<size_t> render_fill_no_triangles_last_frame{0};
    std::atomic<size_t> render_fill_bad_indices_last_frame{0};
    std::atomic<size_t> visible_vacant_parcels_last_frame{0};
    std::atomic<size_t> vacant_parcels_matched_total{0};
    std::atomic<size_t> vacant_parcels_with_geometry_total{0};
    std::atomic<size_t> vacant_parcels_triangulated_renderable_total{0};
    std::unique_ptr<MapProjectionCache> persistent_projection_cache;
    size_t persistent_projection_generation = 0;
    size_t projection_generation = 0;
    LayerFeatureRenderCache feature_render_cache;
    uint64_t feature_render_state_key = 0;
    ParcelRuntimeState parcel_runtime_state;
    ZoningRuntimeState zoning_runtime_state;
    CrimePointRuntimeState crime_point_runtime_state;
    PointLayerRuntimeState point_layer_runtime_state;
    PolylineLayerRuntimeState polyline_layer_runtime_state;
    PolygonArtifactRuntimeState polygon_artifact_runtime_state;
    std::atomic<size_t> prof_projection_world_ring_cache_entries{0};
    std::atomic<size_t> prof_projection_world_extent_cache_entries{0};
    std::atomic<size_t> prof_projection_cache_generation{0};
    std::atomic<size_t> vacant_notice_rows_matched_total{0};
    std::atomic<size_t> vacant_rehab_rows_matched_total{0};
    std::atomic<int> current_zoom_state{12};
    std::atomic<double> current_lon_state{-76.6122};
    std::atomic<double> current_lat_state{39.2904};
    bool gpu_profiler_tab_requested = false;
    bool gpu_profiler_reload_requested = false;
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
    int active_hover_layer_idx = -1;
    int active_click_layer_idx = -1;
    bool hover_inspector_enabled = true;
    loadLayerUiState(
        root,
        layers,
        hover_inspector_enabled,
        &active_hover_layer_idx,
        &active_click_layer_idx,
        &parcel_parameter_mode,
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
        &heatmap_multires_blend);
    heatmap_algo = kAggregateNone;
    heatmap_quality_preset = std::clamp(heatmap_quality_preset, 0, 2);
    if (parcel_layer_idx >= 0) {
        for (size_t i = 0; i < layers.size(); ++i) {
            if (layers[i].file == "property_value_parcels.geojson" && layers[i].enabled) {
                parcel_parameter_mode = 2;
                layers[i].enabled = false;
                continue;
            }
        }
    }
    TimeCubeService time_cube_service(root);
    DuckDbAnalytics duckdb_analytics(root);
    std::mutex status_mutex;
    std::vector<LayerRuntimeState> layer_states(layers.size());
    std::vector<LayerSpatialIndex> layer_spatial(layers.size());
    std::vector<size_t> spatial_index_requested_feature_count(layers.size(), 0);
    std::vector<std::string> spatial_index_requested_signature(layers.size());
    std::vector<OwnerAggregate> owner_aggregates;
    LayerBrowseState layer_browse_state;
    MapFilterState map_filter_state;
    auto& selected_owners = map_filter_state.selected_owners;
    std::vector<QueryMapLayer> query_layers;
    std::vector<QueryHistoryEntry> query_history;
    std::mutex api_control_mutex;
    ApiFilterControlCommand api_filter_control_cmd;
    std::vector<ApiQueryControlCommand> api_query_control_cmds;
    ParcelJurisdictionFilterState parcel_jurisdiction_filter_state;
    const auto& parcel_jurisdiction_options = defaultParcelJurisdictionOptions();
    seedDefaultParcelJurisdictions(parcel_jurisdiction_filter_state);
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
    size_t last_hydrated_seen = 0;

    std::vector<bool> hydration_requested(layers.size(), false);
    std::vector<bool> hydration_required(layers.size(), false);
    initializeEnabledLayerStatesFromPersistedArtifacts(root, layers, parcel_layer_idx, status_mutex, layer_states);
    auto recomputeHydratedCount = [&]() {
        const size_t ready_count = countReadyLayerStates(status_mutex, layer_states);
        hydrated_count.store(ready_count, std::memory_order_relaxed);
        last_hydration_progress_at = std::chrono::steady_clock::now();
        last_hydrated_seen = ready_count;
    };
    recomputeHydratedCount();
    auto layer_data_available = [&](size_t idx) -> bool {
        if (idx >= layers.size()) return false;
        std::lock_guard<std::mutex> lk(status_mutex);
        if (idx >= layer_states.size()) return false;
        LayerPipelineStatus st = layer_states[idx].status;
        return st == LayerPipelineStatus::Hydrated ||
               st == LayerPipelineStatus::Ready;
    };
    auto enqueue_hydration = [&](size_t idx, bool required = false) {
        if (idx >= layers.size()) return;
        if (!layers[idx].enabled && !required) return;
        hydration_requested[idx] = true;
        if (required) hydration_required[idx] = true;
        initializeLayerStateFromPersistedArtifacts(root, layers, parcel_layer_idx, status_mutex, layer_states, idx);
        recomputeHydratedCount();
        hydration_requested[idx] = false;
    };

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
        &spatial_mutex,
        &spatial_cv,
        &spatial_jobs,
        &spatial_results
    };
    std::vector<std::thread> hydration_workers;
    std::thread spatial_index_worker = startSpatialIndexWorker(layer_workers_ctx);

    AppBackgroundServices background_services;
    startAppBackgroundServices(AppBackgroundServicesBootstrapInput{
        .app_version = kAppVersion,
        .protocol_version = kProtocolVersion,
        .root = &root,
        .stop = &hydration_stop,
        .status_api = StatusApiContextFactoryInput{
            .app_version = kAppVersion,
            .protocol_version = kProtocolVersion,
            .tile_cache_max = kMaxTileCache,
            .root = &root,
            .stop = &hydration_stop,
            .layers = &layers,
            .parcel_layer_idx = parcel_layer_idx,
            .duckdb_analytics = &duckdb_analytics,
            .unified_parcels = &unified_parcels,
            .map_filter_state = &map_filter_state,
            .active_filter_result_set = &parcel_jurisdiction_filter_state.result_set,
            .query_layers = &query_layers,
            .active_filter_status = &parcel_jurisdiction_filter_state.status,
            .time_cube_service = &time_cube_service,
            .screenshot = &g_ScreenshotState,
            .status_mutex = &status_mutex,
            .layer_states = &layer_states,
            .layer_fill_mutex = &layer_fill_mutex,
            .layer_fill_enabled = &layer_fill_enabled,
            .layer_hover_enabled = &layer_hover_enabled,
            .layer_inspect_enabled = &layer_inspect_enabled,
            .layer_heatmap_enabled = &layer_heatmap_enabled,
            .hydration_started_at = &hydration_started_at,
            .hydrated_count = &hydrated_count,
            .prof_tile_cache_size = &prof_tile_cache_size,
            .current_zoom_state = &current_zoom_state,
            .current_lon_state = &current_lon_state,
            .current_lat_state = &current_lat_state,
            .visible_vacant_parcels_last_frame = &visible_vacant_parcels_last_frame,
            .vacant_parcels_matched_total = &vacant_parcels_matched_total,
            .vacant_parcels_with_geometry_total = &vacant_parcels_with_geometry_total,
            .perf_frame_ms_avg = &perf_frame_ms_avg,
            .perf_frame_ms_last = &perf_frame_ms_last,
            .perf_fps_avg = &perf_fps_avg,
            .ui_left_panel_frac = &ui_left_panel_frac,
            .ui_right_panel_frac = &ui_right_panel_frac,
            .render_fill_attempts_last_frame = &render_fill_attempts_last_frame,
            .render_fill_success_last_frame = &render_fill_success_last_frame,
            .render_fill_no_triangles_last_frame = &render_fill_no_triangles_last_frame,
            .render_fill_bad_indices_last_frame = &render_fill_bad_indices_last_frame,
            .api_layer_mutex = &api_layer_mutex,
            .api_layer_enable_cmds = &api_layer_enable_cmds,
            .api_layer_fill_cmds = &api_layer_fill_cmds,
            .api_layer_download_cmds = &api_layer_download_cmds,
            .api_zoom_cmd = &api_zoom_cmd,
            .api_lon_cmd = &api_lon_cmd,
            .api_lat_cmd = &api_lat_cmd,
            .api_ui_cmd_seq = &api_ui_cmd_seq,
            .api_ui_cmd_kind = &api_ui_cmd_kind,
            .api_ui_cmd_x = &api_ui_cmd_x,
            .api_ui_cmd_y = &api_ui_cmd_y,
            .api_ui_cmd_button = &api_ui_cmd_button,
            .api_ui_cmd_scroll_y = &api_ui_cmd_scroll_y,
            .api_control_mutex = &api_control_mutex,
            .api_filter_control_cmd = &api_filter_control_cmd,
            .api_query_control_cmds = &api_query_control_cmds,
            .layer_profile_mutex = &layer_profile_mutex,
            .layer_profile_snapshot = &layer_profile_snapshot,
            .profile_mutex = &profile_mutex,
            .profile_samples = &profile_samples,
            .profile_sample_pos = &profile_sample_pos,
            .profile_sample_count = &profile_sample_count,
            .profile_reset_generation = &profile_reset_generation,
            .prof_ui_ms_last = &prof_ui_ms_last,
            .prof_owner_ms_last = &prof_owner_ms_last,
            .prof_tile_ms_last = &prof_tile_ms_last,
            .prof_layer_ms_last = &prof_layer_ms_last,
            .prof_owner_filter_ms_last = &prof_owner_filter_ms_last,
            .prof_heatmap_ms_last = &prof_heatmap_ms_last,
            .prof_overlay_ms_last = &prof_overlay_ms_last,
            .prof_present_ms_last = &prof_present_ms_last,
            .prof_tiles_drawn_last = &prof_tiles_drawn_last,
            .prof_features_considered_last = &prof_features_considered_last,
            .prof_features_drawn_last = &prof_features_drawn_last,
            .prof_owner_filter_candidates_last = &prof_owner_filter_candidates_last,
            .prof_owner_filter_matches_last = &prof_owner_filter_matches_last,
            .prof_heat_samples_last = &prof_heat_samples_last,
            .prof_retired_textures = &prof_retired_textures,
            .prof_projection_world_ring_cache_entries = &prof_projection_world_ring_cache_entries,
            .prof_projection_world_extent_cache_entries = &prof_projection_world_extent_cache_entries,
            .prof_projection_cache_generation = &prof_projection_cache_generation,
            .prof_heatmap_gpu_splat_active = &prof_heatmap_gpu_splat_active,
            .prof_heatmap_high_quality = &prof_heatmap_high_quality,
            .prof_heatmap_cache_valid = &prof_heatmap_cache_valid,
            .prof_heatmap_texture_resident = &prof_heatmap_texture_resident,
            .prof_heatmap_async_inflight = &prof_heatmap_async_inflight,
            .prof_heatmap_cache_key = &prof_heatmap_cache_key,
            .prof_heatmap_texture_cache_entries = &prof_heatmap_texture_cache_entries
        }
    }, background_services);



    double zoom = 12.0;
    double center_lon = -76.6122;
    double center_lat = 39.2904;
    float ui_text_scale = 1.0f;
    uint64_t api_ui_cmd_last_seq = 0;
    bool api_ui_mouse_release_pending = false;
    int api_ui_mouse_release_button = 0;
    std::vector<bool> last_enabled_state;
    last_enabled_state.reserve(layers.size());
    for (const auto& l : layers) last_enabled_state.push_back(l.enabled);
    if (active_hover_layer_idx < 0 || (size_t)active_hover_layer_idx >= layers.size()) {
        if (parcel_layer_idx >= 0) active_hover_layer_idx = parcel_layer_idx;
        else if (zoning_layer_idx >= 0) active_hover_layer_idx = zoning_layer_idx;
    }
    if (active_click_layer_idx < 0 || (size_t)active_click_layer_idx >= layers.size()) {
        if (parcel_layer_idx >= 0) active_click_layer_idx = parcel_layer_idx;
        else if (zoning_layer_idx >= 0) active_click_layer_idx = zoning_layer_idx;
    }
    if (active_hover_layer_idx >= 0 &&
        active_click_layer_idx >= 0 &&
        (size_t)active_hover_layer_idx < layers.size() &&
        (size_t)active_click_layer_idx < layers.size()) {
        const LayerDef& hover_layer = layers[(size_t)active_hover_layer_idx];
        const LayerDef& click_layer = layers[(size_t)active_click_layer_idx];
        const bool hover_parcel =
            hover_layer.scale == "parcel" &&
            !layerUsesPointGeometry(hover_layer) &&
            !layerUsesPolylineGeometry(hover_layer);
        const bool click_parcel =
            click_layer.scale == "parcel" &&
            !layerUsesPointGeometry(click_layer) &&
            !layerUsesPolylineGeometry(click_layer);
        if (hover_parcel && click_parcel && active_hover_layer_idx != active_click_layer_idx) {
            std::fprintf(
                stderr,
                "[worldsim3] parcel hover/click targets differ hover_idx=%d hover_file=%s click_idx=%d click_file=%s\n",
                active_hover_layer_idx,
                hover_layer.file.c_str(),
                active_click_layer_idx,
                click_layer.file.c_str());
            active_click_layer_idx = active_hover_layer_idx;
            std::fprintf(
                stderr,
                "[worldsim3] parcel click target synced to hover target idx=%d file=%s\n",
                active_click_layer_idx,
                hover_layer.file.c_str());
        }
    }
    hover_inspector_enabled = active_hover_layer_idx >= 0;
    int last_active_hover_layer_idx = active_hover_layer_idx;
    int last_active_click_layer_idx = active_click_layer_idx;
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
            local_layer_exists_cache[i] = layerRuntimeSourceMaterialized(root, layers[i]);
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
    std::vector<LayerDownloadTask> layer_download_active_tasks;
    std::mutex layer_download_item_state_mutex;
    std::unordered_map<size_t, float> layer_download_item_progress;
    std::unordered_map<size_t, std::chrono::steady_clock::time_point> layer_download_item_started_at;
    std::unordered_map<size_t, LayerDownloadEtaState> layer_download_item_eta_state;
    std::unordered_map<size_t, std::string> layer_download_item_status;
    std::unordered_set<size_t> layer_download_item_failed;
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
    ElementInfoUiState element_info_state{
        {},
        (size_t)-1,
        false,
        owner_info_property_query,
        sizeof(owner_info_property_query),
        100,
        12
    };
    ParcelSelectionState parcel_selection;
    bool& show_selected_parcel_details = parcel_selection.show_details;
    std::string& selected_parcel_entity_id = parcel_selection.active_entity_id;
    auto& selected_parcel_entity_ids = parcel_selection.entity_ids;
    auto& selected_parcel_entity_id_set = parcel_selection.entity_id_set;
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
    AppUiStateBootstrapInput ui_state_bootstrap_input{
        &root,
        &layers,
        &layer_browse_state,
        &map_filter_state,
        owner_search_query,
        sizeof(owner_search_query),
        &query_history,
        &center_lon,
        &center_lat,
        &zoom,
        &zoning_layer_idx,
        layer_registry.indices().zoning_layer_idx,
        &parcel_selection
    };
    loadPersistedAppUiState(ui_state_bootstrap_input);
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
    constexpr auto kBasemapCoverageRefreshInterval = std::chrono::seconds(60);
    std::string tile_root_dir_cached = "tiles";
    auto basemap_availability_last_check = std::chrono::steady_clock::time_point{};
    auto last_frame_ts = std::chrono::steady_clock::now();
    double ema_frame_ms = 0.0;
    constexpr double kPerfAlpha = 0.12;
    std::string last_cache_clear_msg;
    bool clear_cache_all = true;
    bool clear_cache_hydration = true;
    bool clear_cache_derived = true;
    bool clear_cache_heatmap_memory = true;
    bool clear_cache_heatmap_disk = true;
    bool clear_cache_tile_memory = true;
    bool clear_cache_tile_disk_presence = true;
    const fs::path cache_root_dir = root / "data" / "cache";
    const fs::path cache_hydration_dir = cache_root_dir / "hydration";
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
        cached_vac_notice_signature.clear();
        cached_vac_rehab_size = 0;
        cached_vac_rehab_signature.clear();
        cached_tax_lien_size = 0;
        cached_tax_lien_signature.clear();
        cached_tax_sale_size = 0;
        cached_tax_sale_signature.clear();
        unified_parcel_cached_size = (size_t)-1;
        unified_real_property_cached_size = (size_t)-1;
        unified_parcel_cached_signature.clear();
        derived_layer_refresh_inputs_signature.clear();
        unified_vacancy_generation_applied = -1;
        unified_tax_generation_applied = -1;
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
        releaseContainerStorage(parcel_blocklot_by_feature);
        parcel_blocklot_cached_signature.clear();
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
    bool map_window_fullscreen = false;
    int map_windowed_x = 0;
    int map_windowed_y = 0;
    int map_windowed_w = initial_window_w;
    int map_windowed_h = initial_window_h;
    auto toggle_map_fullscreen = [&]() {
        if (!map_window_fullscreen) {
            glfwGetWindowPos(window, &map_windowed_x, &map_windowed_y);
            glfwGetWindowSize(window, &map_windowed_w, &map_windowed_h);
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
            if (monitor && mode) {
                glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
                map_window_fullscreen = true;
                g_SwapChainRebuild = true;
            }
        } else {
            glfwSetWindowMonitor(
                window,
                nullptr,
                map_windowed_x,
                map_windowed_y,
                std::max(640, map_windowed_w),
                std::max(420, map_windowed_h),
                0);
            map_window_fullscreen = false;
            g_SwapChainRebuild = true;
        }
    };
    auto request_map_snapshot = [&]() {
        std::lock_guard<std::mutex> lk(g_ScreenshotState.mutex);
        g_ScreenshotState.req_id += 1;
        g_ScreenshotState.pending = true;
        g_ScreenshotState.request_native = false;
        g_ScreenshotState.ok = false;
        g_ScreenshotState.path.clear();
        g_ScreenshotState.error.clear();
        g_ScreenshotState.native_width = 0;
        g_ScreenshotState.native_height = 0;
        g_ScreenshotState.logical_width = 0;
        g_ScreenshotState.logical_height = 0;
        g_ScreenshotState.output_width = 0;
        g_ScreenshotState.output_height = 0;
        g_ScreenshotState.requested_output_width = 3840;
        g_ScreenshotState.requested_output_height = 2160;
        g_ScreenshotState.framebuffer_scale_x = 1.0f;
        g_ScreenshotState.framebuffer_scale_y = 1.0f;
    };
    const fs::path color_editor_dir = root / "data" / "cache" / "ui" / "layer_color_editor";
    const fs::path color_editor_snapshot_path = color_editor_dir / "session.json";
    const fs::path color_editor_command_path = color_editor_dir / "command.json";
    uint64_t color_editor_snapshot_revision = 0;
    uint64_t color_editor_last_command_seq = 0;
    pid_t color_editor_pid = -1;
    int active_color_editor_layer_idx = -1;
    bool active_color_editor_outline_target = false;
    bool pending_external_layer_fill_state_changed = false;
    bool pending_external_layer_heatmap_state_changed = false;
    ParcelRenderCacheBlob& parcel_gpu_render_blob = parcel_runtime_state.render_blob;

    auto collect_numeric_layer_values = [&](const LayerDef& layer) {
        std::vector<double> values;
        if (layer.heatmap_field.empty()) return values;
        values.reserve(layer.features.size());
        for (const auto& fg : layer.features) {
            float v = 0.0f;
            if (tryGetFeaturePropertyFloat(fg, layer.heatmap_field, v) && std::isfinite(v)) values.push_back((double)v);
        }
        return values;
    };
    auto parcel_render_feature_record_at = [&](size_t parcel_idx) -> const ParcelRenderFeatureRecord* {
        if (parcel_idx < parcel_gpu_render_blob.features.size() &&
            parcel_gpu_render_blob.features[parcel_idx].feature_idx == parcel_idx) {
            return &parcel_gpu_render_blob.features[parcel_idx];
        }
        for (const ParcelRenderFeatureRecord& rec : parcel_gpu_render_blob.features) {
            if (rec.feature_idx == parcel_idx) return &rec;
        }
        return nullptr;
    };
    auto triangle_area_sq_m = [](const ImVec2& a, const ImVec2& b, const ImVec2& c) -> double {
        constexpr double kDegToMetersLat = 111320.0;
        const double lat0 = ((double)a.y + (double)b.y + (double)c.y) / 3.0;
        const double sx = kDegToMetersLat * std::cos(lat0 * 3.14159265358979323846 / 180.0);
        const double ax = (double)a.x * sx;
        const double ay = (double)a.y * kDegToMetersLat;
        const double bx = (double)b.x * sx;
        const double by = (double)b.y * kDegToMetersLat;
        const double cx = (double)c.x * sx;
        const double cy = (double)c.y * kDegToMetersLat;
        return std::abs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) * 0.5;
    };
    auto parcel_area_sq_m = [&](size_t parcel_idx) -> double {
        if (const ParcelRenderFeatureRecord* rec = parcel_render_feature_record_at(parcel_idx)) {
            const uint32_t end = rec->index_offset + rec->index_count;
            if (end <= parcel_gpu_render_blob.indices.size()) {
                double total = 0.0;
                for (uint32_t i = rec->index_offset; i + 2 < end; i += 3) {
                    const uint32_t ia = parcel_gpu_render_blob.indices[i];
                    const uint32_t ib = parcel_gpu_render_blob.indices[i + 1];
                    const uint32_t ic = parcel_gpu_render_blob.indices[i + 2];
                    if (ia >= parcel_gpu_render_blob.vertices.size() ||
                        ib >= parcel_gpu_render_blob.vertices.size() ||
                        ic >= parcel_gpu_render_blob.vertices.size()) {
                        continue;
                    }
                    total += triangle_area_sq_m(
                        parcel_gpu_render_blob.vertices[ia],
                        parcel_gpu_render_blob.vertices[ib],
                        parcel_gpu_render_blob.vertices[ic]);
                }
                return total;
            }
        }
        return 0.0;
    };
    auto collect_parcel_area_values = [&](const LayerDef& layer) {
        std::vector<double> values;
        values.reserve(layer.features.size());
        for (size_t i = 0; i < layer.features.size(); ++i) {
            const double total = parcel_area_sq_m(i);
            if (total > 0.0 && std::isfinite(total)) values.push_back(total);
        }
        return values;
    };
    auto collect_parcel_value_per_area_values = [&](const LayerDef& parcel_layer, const LayerDef& property_value_layer) {
        std::vector<double> values;
        const size_t n = std::min(parcel_layer.features.size(), property_value_layer.features.size());
        values.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const double total = parcel_area_sq_m(i);
            if (!(total > 0.0) || !std::isfinite(total)) continue;
            float v = 0.0f;
            if (!tryGetFeaturePropertyFloat(property_value_layer.features[i], property_value_layer.heatmap_field, v) ||
                !std::isfinite(v) || v <= 0.0f) {
                continue;
            }
            values.push_back((double)v / total);
        }
        return values;
    };
    auto make_histogram_snapshot = [&](const std::vector<double>& values, float clip_pct) {
        const ApproxHistogram hist = buildApproxHistogram(values, clip_pct);
        ColorEditorHistogramSnapshot snapshot;
        snapshot.valid = hist.valid;
        snapshot.sample_count = hist.sample_count;
        snapshot.min_value = hist.min_value;
        snapshot.max_value = hist.max_value;
        snapshot.clipped_max_value = hist.clipped_max_value;
        snapshot.median_value = hist.median_value;
        snapshot.bin_width = hist.bin_width;
        snapshot.max_bin = hist.max_bin;
        snapshot.plot_bins = hist.plot_bins;
        snapshot.bins = hist.bins;
        snapshot.cumulative_bins = hist.cumulative_bins;
        return snapshot;
    };
    auto active_parcel_continuous_layer_idx = [&]() -> int {
        if (parcel_layer_idx < 0 || parcel_parameter_mode == 1) return -1;
        if (parcel_parameter_mode == 2 || parcel_parameter_mode == 3) {
            for (size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].file == "property_value_parcels.geojson") return (int)i;
            }
        }
        for (size_t i = 0; i < layers.size(); ++i) {
            if ((int)i == parcel_layer_idx) continue;
            const LayerDef& layer = layers[i];
            const bool is_parameter_layer = layer_registry.isParcelHeatmapLayer(i) ||
                (layer.scale == "parcel" && !layer.heatmap_field.empty());
            if (is_parameter_layer && layer.enabled) return (int)i;
        }
        return -1;
    };
    auto clear_parcel_heatmap_layers = [&]() {
        for (size_t i = 0; i < layers.size(); ++i) {
            LayerDef& layer = layers[i];
            if (layer.scale == "parcel" && !layer.heatmap_field.empty()) {
                layer.enabled = false;
                if (i < layer_heatmap_enabled.size()) layer_heatmap_enabled[i] = false;
            }
        }
    };
    auto build_color_editor_snapshot = [&](size_t layer_idx, bool outline_target) {
        ColorEditorSnapshot snapshot;
        if (layer_idx >= layers.size()) return snapshot;
        const LayerDef& layer = layers[layer_idx];
        snapshot.revision = 0;
        snapshot.command_path = color_editor_command_path;
        snapshot.layer_idx = (int)layer_idx;
        snapshot.outline_target = outline_target;
        snapshot.dark_mode = app_settings.dark_mode;
        snapshot.layer_name = layer.name;
        snapshot.layer_file = layer.file;
        snapshot.fill_color = layer.color;
        snapshot.outline_color = layer.outline_color;

        ColorEditorOptionSnapshot static_option;
        static_option.id = "static";
        static_option.label = "Static";
        snapshot.options.push_back(static_option);

        if ((int)layer_idx == parcel_layer_idx) {
            snapshot.supports_continuous = true;
            ColorEditorOptionSnapshot area_option;
            area_option.id = "parcel_area";
            area_option.label = "Continuous: parcel area";
            area_option.field_label = "parcel_area_sq_m";
            area_option.control_layer_idx = parcel_layer_idx;
            if ((size_t)parcel_layer_idx < layer_normalize_mode.size()) area_option.normalize_mode = layer_normalize_mode[(size_t)parcel_layer_idx];
            if ((size_t)parcel_layer_idx < layer_heatmap_percentile_clip.size()) area_option.percentile_clip = layer_heatmap_percentile_clip[(size_t)parcel_layer_idx];
            if ((size_t)parcel_layer_idx < layer_choropleth_gamma.size()) area_option.gamma = layer_choropleth_gamma[(size_t)parcel_layer_idx];
            area_option.histogram = make_histogram_snapshot(collect_parcel_area_values(layer), area_option.percentile_clip);
            snapshot.options.push_back(area_option);
            const int property_value_layer_idx = layer_registry.findLayerByFile("property_value_parcels.geojson");
            if (property_value_layer_idx >= 0 && (size_t)property_value_layer_idx < layers.size()) {
                ColorEditorOptionSnapshot value_per_area_option;
                value_per_area_option.id = "value_per_area";
                value_per_area_option.label = "Continuous: value per area";
                value_per_area_option.field_label = "value_per_area";
                value_per_area_option.control_layer_idx = property_value_layer_idx;
                if ((size_t)property_value_layer_idx < layer_normalize_mode.size()) value_per_area_option.normalize_mode = layer_normalize_mode[(size_t)property_value_layer_idx];
                if ((size_t)property_value_layer_idx < layer_heatmap_percentile_clip.size()) value_per_area_option.percentile_clip = layer_heatmap_percentile_clip[(size_t)property_value_layer_idx];
                if ((size_t)property_value_layer_idx < layer_choropleth_gamma.size()) value_per_area_option.gamma = layer_choropleth_gamma[(size_t)property_value_layer_idx];
                value_per_area_option.histogram = make_histogram_snapshot(
                    collect_parcel_value_per_area_values(layer, layers[(size_t)property_value_layer_idx]),
                    value_per_area_option.percentile_clip);
                snapshot.options.push_back(value_per_area_option);
            }
            for (size_t i = 0; i < layers.size(); ++i) {
                if ((int)i == parcel_layer_idx) continue;
                const LayerDef& parameter_layer = layers[i];
                const bool is_parameter_layer = layer_registry.isParcelHeatmapLayer(i) ||
                    (parameter_layer.scale == "parcel" && !parameter_layer.heatmap_field.empty());
                if (!is_parameter_layer) continue;
                ColorEditorOptionSnapshot option;
                option.id = "layer:" + std::to_string(i);
                option.label = "Continuous: " + parameter_layer.name;
                option.field_label = parameter_layer.heatmap_field;
                option.control_layer_idx = (int)i;
                if (i < layer_normalize_mode.size()) option.normalize_mode = layer_normalize_mode[i];
                if (i < layer_heatmap_percentile_clip.size()) option.percentile_clip = layer_heatmap_percentile_clip[i];
                if (i < layer_choropleth_gamma.size()) option.gamma = layer_choropleth_gamma[i];
                option.histogram = make_histogram_snapshot(collect_numeric_layer_values(parameter_layer), option.percentile_clip);
                snapshot.options.push_back(option);
            }
            if (parcel_parameter_mode == 1) snapshot.selected_option_id = "parcel_area";
            else if (parcel_parameter_mode == 3) snapshot.selected_option_id = "value_per_area";
            else if (const int active_idx = active_parcel_continuous_layer_idx(); active_idx >= 0) snapshot.selected_option_id = "layer:" + std::to_string(active_idx);
            else snapshot.selected_option_id = "static";
        } else if (!layer.heatmap_field.empty()) {
            snapshot.supports_continuous = true;
            ColorEditorOptionSnapshot option;
            option.id = "self";
            option.label = "Continuous: " + layer.heatmap_field;
            option.field_label = layer.heatmap_field;
            option.control_layer_idx = (int)layer_idx;
            if (layer_idx < layer_normalize_mode.size()) option.normalize_mode = layer_normalize_mode[layer_idx];
            if (layer_idx < layer_heatmap_percentile_clip.size()) option.percentile_clip = layer_heatmap_percentile_clip[layer_idx];
            if (layer_idx < layer_choropleth_gamma.size()) option.gamma = layer_choropleth_gamma[layer_idx];
            option.histogram = make_histogram_snapshot(collect_numeric_layer_values(layer), option.percentile_clip);
            snapshot.options.push_back(option);
            snapshot.selected_option_id = (layer_idx < layer_heatmap_use_gradient.size() && layer_heatmap_use_gradient[layer_idx]) ? "self" : "static";
        }
        return snapshot;
    };
    auto histogram_snapshot_equal = [](const ColorEditorHistogramSnapshot& a, const ColorEditorHistogramSnapshot& b) {
        return a.valid == b.valid &&
               a.sample_count == b.sample_count &&
               a.min_value == b.min_value &&
               a.max_value == b.max_value &&
               a.clipped_max_value == b.clipped_max_value &&
               a.median_value == b.median_value &&
               a.bin_width == b.bin_width &&
               a.max_bin == b.max_bin &&
               a.plot_bins == b.plot_bins &&
               a.bins == b.bins &&
               a.cumulative_bins == b.cumulative_bins;
    };
    auto option_snapshot_equal = [&](const ColorEditorOptionSnapshot& a, const ColorEditorOptionSnapshot& b) {
        return a.id == b.id &&
               a.label == b.label &&
               a.field_label == b.field_label &&
               a.control_layer_idx == b.control_layer_idx &&
               a.normalize_mode == b.normalize_mode &&
               a.percentile_clip == b.percentile_clip &&
               a.gamma == b.gamma &&
               histogram_snapshot_equal(a.histogram, b.histogram);
    };
    auto color_editor_snapshot_equal = [&](const ColorEditorSnapshot& a, const ColorEditorSnapshot& b) {
        if (a.command_path != b.command_path ||
            a.layer_idx != b.layer_idx ||
            a.outline_target != b.outline_target ||
            a.dark_mode != b.dark_mode ||
            a.supports_continuous != b.supports_continuous ||
            a.layer_name != b.layer_name ||
            a.layer_file != b.layer_file ||
            a.fill_color.x != b.fill_color.x || a.fill_color.y != b.fill_color.y ||
            a.fill_color.z != b.fill_color.z || a.fill_color.w != b.fill_color.w ||
            a.outline_color.x != b.outline_color.x || a.outline_color.y != b.outline_color.y ||
            a.outline_color.z != b.outline_color.z || a.outline_color.w != b.outline_color.w ||
            a.selected_option_id != b.selected_option_id ||
            a.options.size() != b.options.size()) {
            return false;
        }
        for (size_t i = 0; i < a.options.size(); ++i) {
            if (!option_snapshot_equal(a.options[i], b.options[i])) return false;
        }
        return true;
    };
    std::optional<ColorEditorSnapshot> last_saved_color_editor_snapshot;
    auto save_color_editor_snapshot_if_changed = [&](ColorEditorSnapshot snapshot) {
        if (last_saved_color_editor_snapshot &&
            color_editor_snapshot_equal(snapshot, *last_saved_color_editor_snapshot)) {
            return true;
        }
        snapshot.revision = ++color_editor_snapshot_revision;
        if (!saveColorEditorSnapshot(color_editor_snapshot_path, snapshot)) return false;
        last_saved_color_editor_snapshot = std::move(snapshot);
        return true;
    };
    auto color_editor_process_alive = [&]() {
        if (color_editor_pid <= 0) return false;
        int status = 0;
        const pid_t waited = waitpid(color_editor_pid, &status, WNOHANG);
        if (waited == 0) return kill(color_editor_pid, 0) == 0;
        color_editor_pid = -1;
        active_color_editor_layer_idx = -1;
        return false;
    };
    auto spawn_color_editor_process = [&]() {
        if (color_editor_process_alive()) return;
        const fs::path exe_path = argc > 0 && argv && argv[0] ? fs::absolute(argv[0]) : fs::path{};
        if (exe_path.empty()) return;
        const pid_t child_pid = fork();
        if (child_pid == 0) {
            execl(
                exe_path.c_str(),
                exe_path.c_str(),
                "--color-editor",
                color_editor_snapshot_path.c_str(),
                (char*)nullptr);
            _exit(127);
        }
        if (child_pid > 0) color_editor_pid = child_pid;
    };
    auto apply_color_editor_option = [&](int layer_idx, const std::string& option_id) {
        if (layer_idx < 0 || (size_t)layer_idx >= layers.size()) return;
        if (layer_idx == parcel_layer_idx) {
            if (option_id == "static") {
                parcel_parameter_mode = 0;
                clear_parcel_heatmap_layers();
                pending_external_layer_heatmap_state_changed = true;
                return;
            }
            if (option_id == "parcel_area") {
                parcel_parameter_mode = 1;
                clear_parcel_heatmap_layers();
                pending_external_layer_heatmap_state_changed = true;
                return;
            }
            if (option_id == "value_per_area") {
                clear_parcel_heatmap_layers();
                parcel_parameter_mode = 3;
                const int property_value_layer_idx = layer_registry.findLayerByFile("property_value_parcels.geojson");
                if (property_value_layer_idx >= 0 && (size_t)property_value_layer_idx < layers.size()) {
                    layers[(size_t)property_value_layer_idx].enabled = true;
                    if ((size_t)property_value_layer_idx < layer_heatmap_enabled.size()) layer_heatmap_enabled[(size_t)property_value_layer_idx] = true;
                    if ((size_t)property_value_layer_idx < layer_heatmap_use_gradient.size()) layer_heatmap_use_gradient[(size_t)property_value_layer_idx] = true;
                    if ((size_t)property_value_layer_idx < layer_heatmap_algo.size()) layer_heatmap_algo[(size_t)property_value_layer_idx] = kAggregateMedianChoropleth;
                    enqueue_hydration((size_t)property_value_layer_idx, true);
                }
                pending_external_layer_heatmap_state_changed = true;
                return;
            }
            if (option_id.rfind("layer:", 0) == 0) {
                const int parameter_idx = std::atoi(option_id.c_str() + 6);
                if (parameter_idx < 0 || (size_t)parameter_idx >= layers.size()) return;
                clear_parcel_heatmap_layers();
                parcel_parameter_mode = layers[(size_t)parameter_idx].file == "property_value_parcels.geojson" ? 2 : 0;
                layers[(size_t)parameter_idx].enabled = true;
                if ((size_t)parameter_idx < layer_heatmap_enabled.size()) layer_heatmap_enabled[(size_t)parameter_idx] = true;
                if ((size_t)parameter_idx < layer_heatmap_use_gradient.size()) layer_heatmap_use_gradient[(size_t)parameter_idx] = true;
                if ((size_t)parameter_idx < layer_heatmap_algo.size()) layer_heatmap_algo[(size_t)parameter_idx] = kAggregateMedianChoropleth;
                enqueue_hydration((size_t)parameter_idx, true);
                pending_external_layer_heatmap_state_changed = true;
                return;
            }
        }
        if (option_id == "static") {
            if ((size_t)layer_idx < layer_heatmap_use_gradient.size()) layer_heatmap_use_gradient[(size_t)layer_idx] = false;
            pending_external_layer_heatmap_state_changed = true;
            return;
        }
        if (option_id == "self") {
            if ((size_t)layer_idx < layer_heatmap_use_gradient.size()) layer_heatmap_use_gradient[(size_t)layer_idx] = true;
            if ((size_t)layer_idx < layer_fill_enabled.size()) layer_fill_enabled[(size_t)layer_idx] = true;
            pending_external_layer_heatmap_state_changed = true;
            pending_external_layer_fill_state_changed = true;
        }
    };
    auto apply_color_editor_command = [&](const ColorEditorCommand& command) {
        if (command.layer_idx < 0 || (size_t)command.layer_idx >= layers.size()) return;
        if (command.type == "fill_color") {
            layers[(size_t)command.layer_idx].color = command.color;
            pending_external_layer_fill_state_changed = true;
            return;
        }
        if (command.type == "outline_color") {
            layers[(size_t)command.layer_idx].outline_color = command.color;
            pending_external_layer_fill_state_changed = true;
            return;
        }
        if (command.type == "select_option") {
            apply_color_editor_option(command.layer_idx, command.option_id);
            return;
        }
        const int control_idx = command.control_layer_idx;
        if (control_idx < 0 || (size_t)control_idx >= layer_normalize_mode.size() ||
            (size_t)control_idx >= layer_heatmap_percentile_clip.size() ||
            (size_t)control_idx >= layer_choropleth_gamma.size()) {
            return;
        }
        if (command.type == "normalize_mode") {
            layer_normalize_mode[(size_t)control_idx] = command.normalize_mode;
            pending_external_layer_heatmap_state_changed = true;
        } else if (command.type == "percentile_clip") {
            layer_heatmap_percentile_clip[(size_t)control_idx] = command.percentile_clip;
            pending_external_layer_heatmap_state_changed = true;
        } else if (command.type == "gamma") {
            layer_choropleth_gamma[(size_t)control_idx] = command.gamma;
            pending_external_layer_heatmap_state_changed = true;
        }
    };
    auto open_external_color_editor = [&](size_t layer_idx, bool outline) {
        if ((int)layer_idx == parcel_layer_idx && !outline) {
            enqueue_hydration(layer_idx, true);
            for (size_t i = 0; i < layers.size(); ++i) {
                if ((int)i == parcel_layer_idx) continue;
                const LayerDef& parameter_layer = layers[i];
                const bool is_parameter_layer = layer_registry.isParcelHeatmapLayer(i) ||
                    (parameter_layer.scale == "parcel" && !parameter_layer.heatmap_field.empty());
                if (is_parameter_layer) enqueue_hydration(i, true);
            }
        }
        const ColorEditorSnapshot snapshot = build_color_editor_snapshot(layer_idx, outline);
        if (snapshot.layer_idx < 0) return;
        if (!save_color_editor_snapshot_if_changed(snapshot)) return;
        active_color_editor_layer_idx = (int)layer_idx;
        active_color_editor_outline_target = outline;
        spawn_color_editor_process();
    };

    while (!glfwWindowShouldClose(window)) {
        if (g_VulkanDeviceLost.load(std::memory_order_relaxed)) {
            std::fprintf(stderr, "[worldsim3] Vulkan device lost; closing application cleanly\n");
            glfwSetWindowShouldClose(window, GLFW_TRUE);
            break;
        }
        g_MapPolygonOutlineThickness = app_settings.map_polygon_outline_thickness;
        glfwPollEvents();
        const bool color_editor_alive = color_editor_process_alive();
        ColorEditorCommand color_editor_command;
        if (loadColorEditorCommand(color_editor_command_path, color_editor_command) &&
            color_editor_command.seq > color_editor_last_command_seq) {
            apply_color_editor_command(color_editor_command);
            color_editor_last_command_seq = color_editor_command.seq;
        }

        int fb_w = 0;
        int fb_h = 0;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        if (fb_w > 0 && fb_h > 0 && (fb_w != g_MainWindowData.Width || fb_h != g_MainWindowData.Height)) {
            w = fb_w;
            h = fb_h;
            g_SwapChainRebuild = true;
        }

        if (g_SwapChainRebuild && !g_VulkanDeviceLost.load(std::memory_order_relaxed)) {
            glfwGetFramebufferSize(window, &w, &h);
            if (w > 0 && h > 0) {
                std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
                if (!check_vk_result_allow_device_loss(vkDeviceWaitIdle(g_Device))) {
                    std::fprintf(stderr, "[worldsim3] Vulkan device lost during swapchain rebuild\n");
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                    break;
                }
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
        float layout_margin = frame_layout.layout_margin;
        const float layout_gap = frame_layout.layout_gap;
        float left_panel_w = frame_layout.left_panel_w;
        float right_panel_w = frame_layout.right_panel_w;
        float map_w = frame_layout.map_w;
        float map_x = frame_layout.map_x;
        float main_panel_h = frame_layout.main_panel_h;
        if (map_window_fullscreen) {
            left_panel_w = 0.0f;
            right_panel_w = 0.0f;
            map_x = layout_margin;
            map_w = std::max(1.0f, layout_w - layout_margin * 2.0f);
            main_panel_h = std::max(0.0f, layout_h - layout_margin * 2.0f);
        }
        drainRetiredTextures();
        drainRetiredParcelGpuResources();
        const auto prof_frame_begin = std::chrono::steady_clock::now();
        auto prof_ms_since = [](std::chrono::steady_clock::time_point t) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        };
        size_t prof_tiles_drawn_frame = 0;
        size_t prof_features_considered_frame = 0;
        size_t prof_features_drawn_frame = 0;
        bool layer_fill_state_changed = pending_external_layer_fill_state_changed;
        bool layer_hover_state_changed = false;
        bool layer_inspect_state_changed = false;
        bool layer_heatmap_state_changed = pending_external_layer_heatmap_state_changed;
        bool heatmap_settings_state_changed = false;
        bool heatmap_controls_active = false;
        pending_external_layer_fill_state_changed = false;
        pending_external_layer_heatmap_state_changed = false;
        if (color_editor_alive &&
            active_color_editor_layer_idx >= 0 &&
            (size_t)active_color_editor_layer_idx < layers.size()) {
            save_color_editor_snapshot_if_changed(
                build_color_editor_snapshot((size_t)active_color_editor_layer_idx, active_color_editor_outline_target));
        }
        FramePreludeResult frame_prelude = runFramePrelude(FramePreludeContext{
            &root,
            &layers,
            &layer_profile_accumulators,
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
            &layer_download_active_tasks,
            &layer_download_item_state_mutex,
            &layer_download_item_progress,
            &layer_download_item_started_at,
            &layer_download_item_eta_state,
            &layer_download_item_status,
            &layer_download_item_failed,
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
            &map_filter_state,
            &parcel_jurisdiction_filter_state.result_set,
            &query_layers,
            &parcel_jurisdiction_filter_state.status,
            &api_control_mutex,
            &api_filter_control_cmd,
            &api_query_control_cmds,
            &parcel_runtime_state.filter_state_key,
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
        LeftPanelResult left_panel;
        if (!map_window_fullscreen) {
            left_panel = drawLeftPanelWindow(LeftPanelContext{
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
                    &layer_browse_state,
	                &map_filter_state,
	                &active_hover_layer_idx,
	                &active_click_layer_idx,
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
                &layer_choropleth_gamma,
                &layer_heatmap_multires_blend,
                &layer_heatmap_zoom_adaptive_bandwidth,
                &layer_heatmap_multires_enabled,
                &layer_heatmap_use_gradient,
                heatmap_algo,
                &layer_fill_mutex,
                &layer_fill_state_changed,
                &layer_hover_state_changed,
                &layer_inspect_state_changed,
                &layer_heatmap_state_changed,
                &heatmap_controls_active,
                parcel_layer_idx,
                crime_nibrs_layer_idx,
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
                &parcel_jurisdiction_filter_state,
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
                kMaxNightSatelliteNativeTileZoom,
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
                [&](size_t i, bool required) { enqueue_hydration(i, required); },
                [&](size_t i, bool outline) { open_external_color_editor(i, outline); }
            });
        }
        if (left_panel.geography_changed) {
            zoning_layer_idx = resolveActiveZoningLayerIndex(
                layers,
                zoning_layer_idx,
                layer_registry.indices().zoning_layer_idx);
        }
        bool zoning_filters_changed = left_panel.zoning_filters_changed;
        bool event_sector_filters_changed = left_panel.event_sector_filters_changed;
        const size_t downloadable_missing_layer_count = left_panel.downloadable_missing_layer_count;
        const size_t queueable_missing_layer_count = left_panel.queueable_missing_layer_count;
        auto resolve_download_label = [&](const std::string& key) -> std::string {
            return resolveDataLibraryDownloadLabel(layer_registry, layers, key);
        };

        PipelineProgressContext pipeline_progress_ctx;
        pipeline_progress_ctx.layer_count = layers.size();
        pipeline_progress_ctx.hydrated_count = &hydrated_count;
        pipeline_progress_ctx.last_hydrated_seen = &last_hydrated_seen;
        pipeline_progress_ctx.last_hydration_progress_at = &last_hydration_progress_at;
        pipeline_progress_ctx.hydration_started_at = hydration_started_at;
        pipeline_progress_ctx.hydrated_mutex = &hydrated_mutex;
        pipeline_progress_ctx.hydrated_queue = &hydrated_queue;
        const PipelineProgressSnapshot pipeline_progress = updatePipelineProgress(pipeline_progress_ctx);
        const size_t hydrated_now = pipeline_progress.hydrated_now;
        const size_t ready_now = pipeline_progress.ready_now;
        const size_t hydrated_pending = pipeline_progress.hydrated_pending;
        const float hydrated_frac = pipeline_progress.hydrated_frac;
        const float ready_frac = pipeline_progress.ready_frac;
        const double elapsed_s = pipeline_progress.elapsed_s;
        const double hydrate_idle_s = pipeline_progress.hydrate_idle_s;

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
        drawGearPanel(
            &show_sources_panel,
            root,
            &app_settings,
            main_imgui_context,
            download_queue_imgui_context,
            bootstrap,
            [&]() { rescanDataLibraryLocalFiles(data_library_ctx); });
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
        data_library_ui_ctx.get_layer_download_snapshot = [&](size_t idx) {
            return layerDownloadItemSnapshot(frame_prelude.layer_download, idx);
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
            ready_now,
            hydrated_pending,
            hydrated_frac,
            ready_frac,
            elapsed_s,
            hydrate_idle_s,
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
            &clear_cache_derived,
            &clear_cache_heatmap_memory,
            &clear_cache_heatmap_disk,
            &clear_cache_tile_memory,
            &clear_cache_tile_disk_presence,
            &last_cache_clear_msg,
            &cache_hydration_dir,
            &cache_derived_dir,
            &cache_aggregate_dir,
            &layers,
            &layer_spatial,
            &layer_profile_accumulators,
            &layer_profile_dirty,
            &layer_states,
            &hydrated_mutex,
            &hydrated_queue,
            &spatial_mutex,
            &spatial_jobs,
            &spatial_results,
            &spatial_cv,
            &spatial_index_requested_feature_count,
            &spatial_index_requested_signature,
            &hydrate_req_mutex,
            &hydrate_requests,
            &hydration_requested,
            &hydration_required,
            &status_mutex,
            parcel_layer_idx,
            vacant_notice_layer_idx,
            vacant_rehab_layer_idx,
            &hydrated_count,
            [&](size_t idx, bool required) { enqueue_hydration(idx, required); },
            [&](const fs::path& p) { return clear_cache_tree(p); },
            [&]() { clear_heatmap_runtime_cache(); },
            [&]() { reset_derived_cache_state(); },
            [&]() { trimProcessHeap(); },
            []() {}
        });
        hover_inspector_enabled = active_hover_layer_idx >= 0;
        const LayerUiStateSyncResult ui_state_sync = syncLayerUiState(LayerUiStateSyncContext{
            &root,
            &layers,
            active_hover_layer_idx,
            active_click_layer_idx,
            &last_active_hover_layer_idx,
            &last_active_click_layer_idx,
            &last_enabled_state,
            zoning_filters_changed,
            event_sector_filters_changed,
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
            &selected_owners,
            &map_filter_state.event_sector_enabled
        });
        const bool vacant_layer_active = ui_state_sync.vacant_layer_active;
        LayerPipelineDrainContext pipeline_drain_ctx;
        pipeline_drain_ctx.layers = &layers;
        pipeline_drain_ctx.hydrated_queue = &hydrated_queue;
        pipeline_drain_ctx.hydrated_mutex = &hydrated_mutex;
        pipeline_drain_ctx.spatial_results = &spatial_results;
        pipeline_drain_ctx.spatial_mutex = &spatial_mutex;
        pipeline_drain_ctx.layer_states = &layer_states;
        pipeline_drain_ctx.layer_spatial = &layer_spatial;
        pipeline_drain_ctx.layer_profile_accumulators = &layer_profile_accumulators;
        pipeline_drain_ctx.spatial_index_requested_feature_count = &spatial_index_requested_feature_count;
        pipeline_drain_ctx.spatial_index_requested_signature = &spatial_index_requested_signature;
        pipeline_drain_ctx.status_mutex = &status_mutex;
        pipeline_drain_ctx.hydration_requested = &hydration_requested;
        pipeline_drain_ctx.hydrate_req_mutex = &hydrate_req_mutex;
        pipeline_drain_ctx.layer_profile_dirty = &layer_profile_dirty;
        pipeline_drain_ctx.hydrated_count = &hydrated_count;
        pipeline_drain_ctx.projection_cache_generation = &projection_generation;
        pipeline_drain_ctx.parcel_layer_idx = parcel_layer_idx;
        pipeline_drain_ctx.vacant_layer_active = vacant_layer_active;
        pipeline_drain_ctx.trim_process_heap = [&]() { trimProcessHeap(); };
        drainSpatialIndexResults(pipeline_drain_ctx);

        DerivedLayerCachesContext derived_layer_caches_ctx;
        derived_layer_caches_ctx.root = &root;
        derived_layer_caches_ctx.layers = &layers;
        derived_layer_caches_ctx.layer_states = &layer_states;
        derived_layer_caches_ctx.app_settings = &app_settings;
        derived_layer_caches_ctx.duckdb_analytics = &duckdb_analytics;
        derived_layer_caches_ctx.zoning_layer_idx = zoning_layer_idx;
        derived_layer_caches_ctx.real_property_layer_idx = real_property_layer_idx;
        derived_layer_caches_ctx.vacant_notice_layer_idx = vacant_notice_layer_idx;
        derived_layer_caches_ctx.vacant_rehab_layer_idx = vacant_rehab_layer_idx;
        derived_layer_caches_ctx.tax_lien_layer_idx = tax_lien_layer_idx;
        derived_layer_caches_ctx.tax_sale_layer_idx = tax_sale_layer_idx;
        derived_layer_caches_ctx.parcel_layer_idx = parcel_layer_idx;
        derived_layer_caches_ctx.parcel_render_blob = &parcel_runtime_state.render_blob;
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
        derived_layer_caches_ctx.harmonized_real_property_features = &harmonized_real_property_features;
        derived_layer_caches_ctx.harmonized_real_property_source_files = &harmonized_real_property_source_files;
        derived_layer_caches_ctx.harmonized_real_property_signature = &harmonized_real_property_signature;
        derived_layer_caches_ctx.cached_real_property_size = &cached_real_property_size;
        derived_layer_caches_ctx.cached_vac_notice_size = &cached_vac_notice_size;
        derived_layer_caches_ctx.cached_vac_notice_signature = &cached_vac_notice_signature;
        derived_layer_caches_ctx.cached_vac_rehab_size = &cached_vac_rehab_size;
        derived_layer_caches_ctx.cached_vac_rehab_signature = &cached_vac_rehab_signature;
        derived_layer_caches_ctx.cached_tax_lien_size = &cached_tax_lien_size;
        derived_layer_caches_ctx.cached_tax_lien_signature = &cached_tax_lien_signature;
        derived_layer_caches_ctx.cached_tax_sale_size = &cached_tax_sale_size;
        derived_layer_caches_ctx.cached_tax_sale_signature = &cached_tax_sale_signature;
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
        derived_layer_caches_ctx.parcel_blocklot_by_feature = &parcel_blocklot_by_feature;
        derived_layer_caches_ctx.parcel_blocklot_cached_signature = &parcel_blocklot_cached_signature;
        derived_layer_caches_ctx.vacant_notice_rows_matched_total = &vacant_notice_rows_matched_total;
        derived_layer_caches_ctx.vacant_rehab_rows_matched_total = &vacant_rehab_rows_matched_total;
        derived_layer_caches_ctx.vacant_parcels_matched_total = &vacant_parcels_matched_total;
        derived_layer_caches_ctx.vacant_parcels_with_geometry_total = &vacant_parcels_with_geometry_total;
        derived_layer_caches_ctx.vacant_parcels_triangulated_renderable_total = &vacant_parcels_triangulated_renderable_total;
        derived_layer_caches_ctx.unified_parcels = &unified_parcels;
        derived_layer_caches_ctx.parcel_owner_search_by_feature = &parcel_owner_search_by_feature;
        derived_layer_caches_ctx.real_property_owner_search_by_feature = &real_property_owner_search_by_feature;
        derived_layer_caches_ctx.parcel_address_search_by_feature = &parcel_address_search_by_feature;
        derived_layer_caches_ctx.unified_parcel_cached_size = &unified_parcel_cached_size;
        derived_layer_caches_ctx.unified_parcel_cached_signature = &unified_parcel_cached_signature;
        derived_layer_caches_ctx.last_refresh_inputs_signature = &derived_layer_refresh_inputs_signature;
        derived_layer_caches_ctx.unified_real_property_cached_size = &unified_real_property_cached_size;
        derived_layer_caches_ctx.unified_vacancy_generation_applied = &unified_vacancy_generation_applied;
        derived_layer_caches_ctx.unified_tax_generation_applied = &unified_tax_generation_applied;
        derived_layer_caches_ctx.owner_aggregates_dirty = &owner_aggregates_dirty;
        refreshDerivedLayerCaches(derived_layer_caches_ctx);
        refreshOwnerTextFilterState(owner_text_filter_state, OwnerTextFilterRefreshContext{
            &map_filter_state,
            &unified_parcels,
            &parcel_owner_search_by_feature,
            parcel_layer_idx,
            real_property_layer_idx,
            &unified_parcel_cached_signature,
            &harmonized_real_property_signature,
            harmonized_real_property_features.size()
        });
        refreshAddressTextFilterState(address_text_filter_state, AddressTextFilterRefreshContext{
            &map_filter_state,
            &unified_parcels,
            &parcel_address_search_by_feature,
            parcel_layer_idx,
            real_property_layer_idx,
            &unified_parcel_cached_signature
        });
        for (size_t li = 0; li < layers.size(); ++li) {
            LayerPipelineStatus st = LayerPipelineStatus::Queued;
            std::string hydration_signature;
            std::string spatial_index_signature;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if (li < layer_states.size()) {
                    st = layer_states[li].status;
                    hydration_signature = layer_states[li].hydration_source_signature;
                    spatial_index_signature = layer_states[li].spatial_index_source_signature;
                }
            }
            const bool stable =
                st == LayerPipelineStatus::Hydrated ||
                st == LayerPipelineStatus::Ready;
            if (!stable) continue;
            const size_t feature_count = layers[li].features.size();
            if (feature_count == 0) continue;
            const bool spatial_index_current =
                layer_spatial[li].built &&
                layer_spatial[li].feature_count_built == feature_count &&
                spatial_index_signature == hydration_signature;
            if (spatial_index_current) continue;
            if (spatial_index_requested_feature_count[li] == feature_count &&
                spatial_index_requested_signature[li] == hydration_signature) {
                continue;
            }
            SpatialIndexJob job;
            job.index = li;
            job.source_signature = hydration_signature;
            job.feature_extents.reserve(feature_count);
            for (const auto& fg : layers[li].features) job.feature_extents.push_back(fg.extent);
            {
                std::lock_guard<std::mutex> lk(spatial_mutex);
                spatial_jobs.push_back(std::move(job));
            }
            spatial_index_requested_feature_count[li] = feature_count;
            spatial_index_requested_signature[li] = hydration_signature;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if (li < layer_states.size()) {
                    layer_states[li].spatial_index_source_signature = hydration_signature;
                    layer_states[li].spatial_index_phase = "queued";
                }
            }
            spatial_cv.notify_one();
        }
        refreshLayerProfileSnapshot(frame_prelude.layer_profile_snapshot);
        if (consumeGpuProfilerTabSelectionRequest()) {
            gpu_profiler_tab_requested = true;
        }

        auto make_frame_filter_input = [&]() {
            FeatureFilterContextFactoryInput filter_input;
            filter_input.layers = &layers;
            filter_input.map_filters = &map_filter_state;
            filter_input.result_set = parcel_jurisdiction_filter_state.result_set.active
                ? &parcel_jurisdiction_filter_state.result_set
                : nullptr;
            filter_input.secondary_result_set = owner_text_filter_state.result_set.active
                ? &owner_text_filter_state.result_set
                : nullptr;
            filter_input.tertiary_result_set = address_text_filter_state.result_set.active
                ? &address_text_filter_state.result_set
                : nullptr;
            filter_input.query_layers = &query_layers;
            filter_input.unified_parcels = &unified_parcels;
            filter_input.parcel_owner_search_by_feature = &parcel_owner_search_by_feature;
            filter_input.real_property_owner_search_by_feature = &real_property_owner_search_by_feature;
            filter_input.parcel_address_search_by_feature = &parcel_address_search_by_feature;
            filter_input.real_property_by_blocklot = &real_property_by_blocklot;
            filter_input.parcel_vac_notice_by_feature = &parcel_vac_notice_by_feature;
            filter_input.parcel_vac_rehab_by_feature = &parcel_vac_rehab_by_feature;
            filter_input.real_property_layer_idx = real_property_layer_idx;
            filter_input.parcel_layer_idx = parcel_layer_idx;
            filter_input.crime_nibrs_layer_idx = crime_nibrs_layer_idx;
            filter_input.compiled_owner_filter_active = filter_input.secondary_result_set != nullptr;
            filter_input.compiled_address_filter_active = filter_input.tertiary_result_set != nullptr;
            return filter_input;
        };
        auto ensure_frame_feature_render_cache = [&]() -> const LayerFeatureRenderCache& {
            FeatureFilterContextFactoryInput filter_input = make_frame_filter_input();
            FeatureRenderStateKeyContext key_ctx;
            key_ctx.map_filters = &map_filter_state;
            key_ctx.result_set = filter_input.result_set;
            key_ctx.secondary_result_set = filter_input.secondary_result_set;
            key_ctx.tertiary_result_set = filter_input.tertiary_result_set;
            key_ctx.query_layers = &query_layers;
            feature_render_state_key = buildFeatureRenderStateKey(key_ctx);
            const FeatureFilterContext filter_ctx = makeFeatureFilterContext(filter_input);
            ensureLayerFeatureRenderCache(
                filter_ctx,
                layers,
                feature_render_state_key,
                feature_render_cache);
            return feature_render_cache;
        };

        syncParcelGpuLayer(
            ParcelRuntimeSyncInput{
                .root = &root,
                .app_settings = &app_settings,
                .layers = &layers,
                .layer_states = &layer_states,
                .parcel_layer_idx = parcel_layer_idx,
                .property_value_layer_idx = layer_registry.findLayerByFile("property_value_parcels.geojson"),
                .vacant_notice_layer_idx = vacant_notice_layer_idx,
                .vacant_rehab_layer_idx = vacant_rehab_layer_idx,
                .tax_lien_layer_idx = tax_lien_layer_idx,
                .tax_sale_layer_idx = tax_sale_layer_idx,
                .parcel_parameter_mode = parcel_parameter_mode,
                .layer_choropleth_gamma = &layer_choropleth_gamma,
                .layer_heatmap_percentile_clip = &layer_heatmap_percentile_clip,
                .layer_normalize_mode = &layer_normalize_mode,
                .layer_fill_enabled = &layer_fill_enabled,
                .map_filter_state = &map_filter_state,
                .parcel_jurisdiction_result_set = &parcel_jurisdiction_filter_state.result_set,
                .query_layers = &query_layers,
                .unified_parcels = &unified_parcels,
                .parcel_vac_notice_by_feature = &parcel_vac_notice_by_feature,
                .parcel_vac_rehab_by_feature = &parcel_vac_rehab_by_feature,
                .parcel_tax_lien_by_feature = &parcel_tax_lien_by_feature,
                .parcel_tax_sale_by_feature = &parcel_tax_sale_by_feature,
                .selected_parcel_id_set = &selected_parcel_entity_id_set,
                .gpu_profiler_reload_requested = &gpu_profiler_reload_requested,
                .feature_render_state_key = feature_render_state_key,
                .ensure_feature_render_cache = [&]() -> const LayerFeatureRenderCache& {
                    return ensure_frame_feature_render_cache();
                },
                .parcel_area_sq_m = [&](size_t parcel_idx) -> double {
                    return parcel_area_sq_m(parcel_idx);
                },
            },
            parcel_runtime_state);

        syncZoningGpuLayers(
            ZoningRuntimeSyncInput{
                .root = &root,
                .app_settings = &app_settings,
                .layers = &layers,
                .layer_states = &layer_states,
                .parcel_layer_idx = parcel_layer_idx,
                .map_filter_state = &map_filter_state,
                .query_layers = &query_layers,
                .zoning_zone_enabled = &zoning_zone_enabled,
                .zoning_zone_color = &zoning_zone_color,
                .layer_fill_enabled = &layer_fill_enabled,
                .layer_heatmap_use_gradient = &layer_heatmap_use_gradient,
                .layer_choropleth_gamma = &layer_choropleth_gamma,
                .layer_normalize_mode = &layer_normalize_mode,
                .layer_heatmap_percentile_clip = &layer_heatmap_percentile_clip,
                .heatmap_percentile_clip = heatmap_percentile_clip,
                .feature_render_state_key = feature_render_state_key,
                .ensure_feature_render_cache = [&]() -> const LayerFeatureRenderCache& {
                    return ensure_frame_feature_render_cache();
                },
            },
            zoning_runtime_state);

        syncCrimePointGpuLayer(
            CrimePointRuntimeSyncInput{
                .root = &root,
                .layers = &layers,
                .layer_states = &layer_states,
                .crime_nibrs_layer_idx = crime_nibrs_layer_idx,
                .map_filter_state = &map_filter_state,
                .query_layers = &query_layers,
                .feature_render_state_key = feature_render_state_key,
                .ensure_feature_render_cache = [&]() -> const LayerFeatureRenderCache& {
                    return ensure_frame_feature_render_cache();
                },
            },
            crime_point_runtime_state);
        publishCrimePointArtifactForAggregateSampling(
            crime_nibrs_layer_idx,
            crime_point_runtime_state,
            point_layer_runtime_state.geometry_artifacts,
            &point_layer_runtime_state.artifact_signatures);

        syncPointGpuLayers(
            PointLayerRuntimeSyncInput{
                .root = &root,
                .layers = &layers,
                .layer_states = &layer_states,
                .map_filter_state = &map_filter_state,
                .query_layers = &query_layers,
                .feature_render_state_key = feature_render_state_key,
                .crime_nibrs_layer_idx = crime_nibrs_layer_idx,
                .ensure_feature_render_cache = [&]() -> const LayerFeatureRenderCache& {
                    return ensure_frame_feature_render_cache();
                },
            },
            point_layer_runtime_state);

        syncPolylineGpuLayers(
            PolylineLayerRuntimeSyncInput{
                .root = &root,
                .layers = &layers,
                .layer_states = &layer_states,
                .map_filter_state = &map_filter_state,
                .query_layers = &query_layers,
                .feature_render_state_key = feature_render_state_key,
                .ensure_feature_render_cache = [&]() -> const LayerFeatureRenderCache& {
                    return ensure_frame_feature_render_cache();
                },
            },
            polyline_layer_runtime_state);

        syncPolygonGeometryArtifacts(
            PolygonArtifactRuntimeSyncInput{
                .root = &root,
                .layers = &layers,
                .layer_states = &layer_states,
                .parcel_layer_idx = parcel_layer_idx,
                .gpu_uploaded_signatures = &zoning_runtime_state.uploaded_signatures,
            },
            polygon_artifact_runtime_state);

        if (!map_window_fullscreen) {
            drawRightPanelWindow(RightPanelContext{
                &root,
                &app_settings,
                &duckdb_analytics,
                layout_w,
                right_panel_w,
                layout_margin,
                main_panel_h,
                map_w,
                &layers,
                &unified_parcels,
                &parcel_runtime_state.render_blob,
                &map_filter_state,
                &query_layers,
                &query_history,
                &zoning_metadata,
                &zoning_zone_enabled,
                &real_property_by_blocklot,
                &selected_owners,
                &selected_parcel_entity_id_set,
                &selected_parcel_entity_ids,
                &parcel_selection,
                &element_info_state,
                &show_selected_parcel_details,
                &show_selected_zone_details,
                &selected_zone_idx,
                &center_lon,
                &center_lat,
                &zoom,
                kMinZoom,
                kMaxZoom,
                parcel_layer_idx,
                zoning_layer_idx,
                real_property_layer_idx,
                crime_nibrs_layer_idx,
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
                &layer_fill_enabled,
                &layer_heatmap_state_changed,
                heatmap_percentile_clip,
                &parcel_vac_notice_by_feature,
                &parcel_vac_rehab_by_feature,
                &parcel_jurisdiction_filter_state,
                &owner_text_filter_state.result_set,
                &address_text_filter_state.result_set,
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
	                &profile_mutex,
	                &profile_samples,
	                &profile_sample_pos,
	                &profile_sample_count,
	                &prof_heatmap_gpu_splat_active,
	                &prof_heatmap_high_quality,
	                &prof_heatmap_texture_resident,
	                &prof_heatmap_async_inflight,
	                &prof_heatmap_texture_cache_entries,
	                &gpu_profiler_tab_requested,
	                &gpu_profiler_reload_requested
	            });
        }

        (void)ensure_frame_feature_render_cache();
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
            &point_layer_runtime_state.geometry_artifacts,
            &polyline_layer_runtime_state.geometry_artifacts,
            &polygon_artifact_runtime_state.geometry_artifacts,
            &parcel_runtime_state.render_blob,
            &layer_spatial,
            &map_filter_state,
            &query_layers,
            &real_property_by_blocklot,
            &zoning_metadata,
            &zoning_zone_enabled,
            &zoning_zone_color,
            &parcel_selection,
            &selected_parcel_entity_ids,
            &show_selected_zone_details,
            &selected_zone_idx,
            &element_info_state,
            &background_services.hover_debug_state,
            real_property_layer_idx,
            parcel_layer_idx,
            zoning_layer_idx,
            crime_nibrs_layer_idx,
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
            parcel_jurisdiction_options.size(),
            &parcel_jurisdiction_filter_state,
            &parcel_vac_notice_by_feature,
            &parcel_vac_rehab_by_feature,
            &parcel_tax_lien_by_feature,
            &parcel_tax_sale_by_feature,
            &parcel_tax_lien_amount_by_feature,
            &parcel_tax_sale_amount_by_feature,
            &unified_parcels,
            &parcel_owner_search_by_feature,
            &real_property_owner_search_by_feature,
            &parcel_address_search_by_feature,
            &owner_text_filter_state.result_set,
            &address_text_filter_state.result_set,
            &feature_render_cache,
            global_heat_cell_px,
            heatmap_algo,
            heatmap_quality_preset,
            heatmap_bandwidth_px,
            heatmap_blur_sigma_px,
            heatmap_percentile_clip,
            heatmap_zoom_adaptive_bandwidth,
            heatmap_multires_enabled,
            heatmap_multires_blend,
            heatmap_controls_active,
            &heatmap_runtime,
            active_hover_layer_idx,
            active_click_layer_idx,
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
            &prof_owner_filter_ms_last,
            &prof_heatmap_ms_last,
            &prof_owner_filter_candidates_last,
            &prof_owner_filter_matches_last,
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
            &persistent_projection_cache,
            &persistent_projection_generation,
            projection_generation,
            &prof_projection_world_ring_cache_entries,
            &prof_projection_world_extent_cache_entries,
            &prof_projection_cache_generation,
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
            toggle_map_fullscreen,
            request_map_snapshot,
            map_window_fullscreen
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
            &prof_owner_filter_ms_last,
            &prof_heatmap_ms_last,
            &prof_overlay_ms_last,
            &prof_present_ms_last,
            &prof_tiles_drawn_last,
            &prof_features_considered_last,
            &prof_features_drawn_last,
            &prof_owner_filter_candidates_last,
            &prof_owner_filter_matches_last,
            &prof_retired_textures,
            &prof_tile_cache_size,
            &prof_heat_samples_last,
            &profile_mutex,
            &profile_samples,
            &profile_sample_pos,
            &profile_sample_count,
            app_settings.dark_mode
        });
        if (g_VulkanDeviceLost.load(std::memory_order_relaxed)) {
            std::fprintf(stderr, "[worldsim3] Vulkan device lost during frame present; exiting main loop\n");
            glfwSetWindowShouldClose(window, GLFW_TRUE);
            continue;
        }

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
            &layer_download_last_event,
            app_settings.dark_mode
        });
    }
    if (color_editor_process_alive()) {
        kill(color_editor_pid, SIGTERM);
        waitpid(color_editor_pid, nullptr, 0);
        color_editor_pid = -1;
    }
    if (!g_VulkanDeviceLost.load(std::memory_order_relaxed)) {
        vkDeviceWaitIdle(g_Device);
    }
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
    shutdown_input.active_hover_layer_idx = &active_hover_layer_idx;
    shutdown_input.active_click_layer_idx = &active_click_layer_idx;
    shutdown_input.parcel_parameter_mode = &parcel_parameter_mode;
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
    shutdown_input.event_sector_enabled = &map_filter_state.event_sector_enabled;
    shutdown_input.center_lon = &center_lon;
    shutdown_input.center_lat = &center_lat;
    shutdown_input.zoom = &zoom;
    shutdown_input.query_history = &query_history;
    shutdown_input.selected_parcel_entity_id = &selected_parcel_entity_id;
    shutdown_input.selected_parcel_entity_ids = &selected_parcel_entity_ids;
    shutdown_input.hydration_stop = &hydration_stop;
    shutdown_input.time_cube_ui_worker = &time_cube_ui_worker;
    shutdown_input.hydrate_req_cv = &hydrate_req_cv;
    shutdown_input.spatial_cv = &spatial_cv;
    shutdown_input.hydration_workers = &hydration_workers;
    shutdown_input.spatial_index_worker = &spatial_index_worker;
    shutdown_input.status_api_worker = &background_services.status_api_worker;
    shutdown_input.dataset_api_worker = &background_services.dataset_api_worker;
    shutdown_input.lan_discovery_worker = &background_services.lan_discovery_worker;
    shutdown_input.heatmap_raster_texture = &heatmap_runtime.raster_texture;
    AppShutdownContext shutdown_ctx = makeAppShutdownContext(shutdown_input);
    shutdownWorldSimApp(shutdown_ctx);
    return 0;
}
