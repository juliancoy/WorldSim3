#pragma once

#include "app_settings.h"
#include "duckdb_analytics.h"
#include "filters.h"
#include "heatmap_runtime.h"
#include "layer_runtime.h"
#include "map_canvas_session.h"
#include "map_frame_session.h"
#include "owner_info.h"
#include "policy_panel.h"
#include "selection.h"
#include "time_cube_panel.h"
#include "types.h"
#include "zoning.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct MapTabContext {
    float map_x = 0.0f;
    float map_w = 0.0f;
    float layout_margin = 0.0f;
    float main_panel_h = 0.0f;

    const std::filesystem::path* root = nullptr;
    const AppSettings* app_settings = nullptr;
    DuckDbAnalytics* duckdb_analytics = nullptr;

    double* center_lon = nullptr;
    double* center_lat = nullptr;
    int* zoom = nullptr;
    int min_zoom = 0;
    int max_zoom = 0;
    int max_internal_math_zoom = 0;

    std::vector<LayerDef>* layers = nullptr;
    std::vector<LayerSpatialIndex>* layer_spatial = nullptr;
    MapFilterState* map_filter_state = nullptr;
    std::vector<QueryMapLayer>* query_layers = nullptr;
    std::unordered_map<std::string, size_t>* real_property_by_blocklot = nullptr;
    std::unordered_map<std::string, ZoneMetadata>* zoning_metadata = nullptr;
    std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr;
    std::unordered_map<std::string, ImVec4>* zoning_zone_color = nullptr;

    ParcelSelectionState* parcel_selection = nullptr;
    std::vector<size_t>* selected_parcel_indices = nullptr;
    bool* show_selected_zone_details = nullptr;
    size_t* selected_zone_idx = nullptr;
    ElementInfoUiState* element_info_state = nullptr;

    int real_property_layer_idx = -1;
    int parcel_layer_idx = -1;
    int zoning_layer_idx = -1;
    int crime_nibrs_layer_idx = -1;
    int crime_legacy_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
    int parcel_parameter_mode = 0;

    const std::vector<bool>* layer_fill_enabled = nullptr;
    const std::vector<bool>* layer_hover_enabled = nullptr;
    const std::vector<bool>* layer_inspect_enabled = nullptr;
    const std::vector<bool>* layer_heatmap_enabled = nullptr;
    const std::vector<int>* layer_heatmap_algo = nullptr;
    const std::vector<int>* layer_heatmap_max_zoom = nullptr;
    const std::vector<int>* layer_parcel_detail_min_zoom = nullptr;
    const std::vector<float>* layer_heatmap_cell_px = nullptr;
    const std::vector<float>* layer_heatmap_bandwidth_px = nullptr;
    const std::vector<float>* layer_heatmap_blur_sigma_px = nullptr;
    const std::vector<float>* layer_heatmap_percentile_clip = nullptr;
    const std::vector<bool>* layer_heatmap_zoom_adaptive_bandwidth = nullptr;
    const std::vector<bool>* layer_heatmap_multires_enabled = nullptr;
    const std::vector<float>* layer_heatmap_multires_blend = nullptr;
    const std::vector<bool>* layer_heatmap_use_gradient = nullptr;
    const std::vector<float>* layer_choropleth_gamma = nullptr;
    const std::vector<int>* layer_normalize_mode = nullptr;

    std::unordered_set<std::string>* parcel_jurisdiction_filter = nullptr;
    size_t parcel_jurisdiction_option_count = 0;
    bool* parcel_jurisdiction_filter_dirty = nullptr;
    FilterResultSet* parcel_jurisdiction_result_set = nullptr;
    std::string* parcel_jurisdiction_filter_status = nullptr;

    std::vector<int>* parcel_vac_notice_by_feature = nullptr;
    std::vector<int>* parcel_vac_rehab_by_feature = nullptr;
    std::vector<int>* parcel_tax_lien_by_feature = nullptr;
    std::vector<int>* parcel_tax_sale_by_feature = nullptr;
    std::vector<double>* parcel_tax_lien_amount_by_feature = nullptr;
    std::vector<double>* parcel_tax_sale_amount_by_feature = nullptr;
    std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;

    float global_heat_cell_px = 24.0f;
    int heatmap_algo = 0;
    int heatmap_quality_preset = 0;
    float heatmap_bandwidth_px = 18.0f;
    float heatmap_blur_sigma_px = 6.0f;
    float heatmap_percentile_clip = 95.0f;
    bool heatmap_zoom_adaptive_bandwidth = true;
    bool heatmap_multires_enabled = true;
    float heatmap_multires_blend = 0.5f;
    bool heatmap_allow_cpu_fallback = false;
    bool heatmap_controls_active = false;
    HeatmapRuntimeState* heatmap_runtime = nullptr;

    int hover_inspector_mode = 0;
    bool* hover_inspector_enabled = nullptr;

    LazyTileDownloadState* lazy_tile_download = nullptr;
    bool* topo_tiles_available_cached = nullptr;
    bool* topo_vector_available_cached = nullptr;
    bool* basemap_source_has_any_files_cached = nullptr;
    std::string* tile_root_dir_cached = nullptr;
    std::chrono::steady_clock::time_point* basemap_availability_last_check = nullptr;
    int max_native_tile_zoom = 18;

    size_t* prof_tiles_drawn_frame = nullptr;
    size_t* prof_features_considered_frame = nullptr;
    size_t* prof_features_drawn_frame = nullptr;
    std::atomic<double>* prof_tile_ms_last = nullptr;
    std::atomic<double>* prof_layer_ms_last = nullptr;
    std::atomic<double>* prof_heatmap_ms_last = nullptr;
    std::atomic<size_t>* prof_heat_samples_last = nullptr;
    std::atomic<bool>* prof_heatmap_gpu_splat_active = nullptr;
    std::atomic<bool>* prof_heatmap_high_quality = nullptr;
    std::atomic<bool>* prof_heatmap_cache_valid = nullptr;
    std::atomic<bool>* prof_heatmap_texture_resident = nullptr;
    std::atomic<bool>* prof_heatmap_async_inflight = nullptr;
    std::atomic<uint64_t>* prof_heatmap_cache_key = nullptr;
    std::atomic<size_t>* prof_heatmap_texture_cache_entries = nullptr;
    std::atomic<size_t>* visible_vacant_parcels_last_frame = nullptr;
    std::atomic<double>* prof_overlay_ms_last = nullptr;
    std::atomic<size_t>* render_fill_attempts_last_frame = nullptr;
    std::atomic<size_t>* render_fill_success_last_frame = nullptr;
    std::atomic<size_t>* render_fill_no_triangles_last_frame = nullptr;
    std::atomic<size_t>* render_fill_bad_indices_last_frame = nullptr;

    TimeCubeService* time_cube_service = nullptr;
    TimeCubeResult* time_cube_ui_result = nullptr;
    bool* time_cube_ui_loaded = nullptr;
    std::string* time_cube_ui_status = nullptr;
    std::mutex* time_cube_ui_mutex = nullptr;
    std::thread* time_cube_ui_worker = nullptr;
    std::atomic<bool>* time_cube_ui_running = nullptr;
    std::atomic<bool>* time_cube_ui_done = nullptr;
    std::vector<bool>* time_cube_selected = nullptr;
    int* time_cube_year_min = nullptr;
    int* time_cube_year_max = nullptr;
    int* time_cube_normalize_mode = nullptr;
    bool* time_cube_show_excluded = nullptr;

    nlohmann::json* policy_hierarchy = nullptr;
    bool policy_hierarchy_loaded = false;
    std::string* policy_hierarchy_error = nullptr;
    char* policy_hierarchy_query = nullptr;
    size_t policy_hierarchy_query_capacity = 0;
    int* policy_hierarchy_scope = nullptr;
    std::vector<PublicServantRosterRow>* public_servant_roster = nullptr;
    std::string* people_pay_cached_query = nullptr;
    int* people_pay_cached_scope = nullptr;
    size_t* people_pay_cache_matched_count = nullptr;
    std::vector<size_t>* people_pay_visible_rows = nullptr;
    size_t* people_pay_cache_rebuilds = nullptr;
    size_t* people_pay_rendered_rows_last = nullptr;
    PolicyVizNode* policy_viz_root = nullptr;
    std::string* policy_viz_cached_query = nullptr;
    int* policy_viz_cached_scope = nullptr;
    int* policy_viz_cached_metric = nullptr;
    int* policy_viz_metric = nullptr;
    size_t* policy_viz_cache_rebuilds = nullptr;
    size_t* policy_viz_node_count = nullptr;

    std::function<const LayerDef::FeatureGeom*(const LayerDef::FeatureGeom&)> real_property_for_parcel;
};

void drawMapTabWindow(const MapTabContext& ctx);
