#pragma once

#include "app_settings.h"
#include "app_utils.h"
#include "active_queries_tab.h"
#include "av_capture.h"
#include "cache_io.h"
#include "duckdb_analytics.h"
#include "filters.h"
#include "filters_tab.h"
#include "gpu_profiler_tab.h"
#include "layer_runtime.h"
#include "owner_aggregates.h"
#include "owner_info.h"
#include "owners_tab.h"
#include "parcel_unified.h"
#include "road_label_state.h"
#include "selection.h"
#include "types.h"
#include "zoning.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct RightPanelContext {
    const std::filesystem::path* root = nullptr;
    AppSettings* app_settings = nullptr;
    DuckDbAnalytics* duckdb_analytics = nullptr;

    float layout_w = 0.0f;
    float right_panel_w = 0.0f;
    float layout_margin = 0.0f;
    float main_panel_h = 0.0f;
    float map_w = 0.0f;

    std::vector<LayerDef>* layers = nullptr;
    std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;
    const ParcelRenderCacheBlob* parcel_render_blob = nullptr;
    MapFilterState* map_filter_state = nullptr;
    std::vector<QueryMapLayer>* query_layers = nullptr;
    std::vector<QueryHistoryEntry>* query_history = nullptr;
    std::unordered_map<std::string, ZoneMetadata>* zoning_metadata = nullptr;
    std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr;
    std::unordered_map<std::string, size_t>* real_property_by_blocklot = nullptr;

    std::unordered_set<std::string>* selected_owners = nullptr;
    std::unordered_set<std::string>* selected_parcel_id_set = nullptr;
    std::vector<std::string>* selected_parcel_ids = nullptr;
    ParcelSelectionState* parcel_selection = nullptr;
    ElementInfoUiState* element_info_state = nullptr;

    bool* show_selected_parcel_details = nullptr;
    bool* show_selected_zone_details = nullptr;
    int* selected_zone_layer_idx = nullptr;
    size_t* selected_zone_idx = nullptr;
    double* center_lon = nullptr;
    double* center_lat = nullptr;
    double* zoom = nullptr;
    int min_zoom = 0;
    int max_zoom = 0;

    int parcel_layer_idx = -1;
    int zoning_layer_idx = -1;
    int real_property_layer_idx = -1;
    int crime_nibrs_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
    int parcel_parameter_mode = 0;
    int heatmap_algo = 0;

    std::vector<bool>* layer_heatmap_enabled = nullptr;
    std::vector<bool>* layer_hover_enabled = nullptr;
    std::vector<bool>* layer_inspect_enabled = nullptr;
    std::vector<int>* layer_heatmap_max_zoom = nullptr;
    std::vector<int>* layer_parcel_detail_min_zoom = nullptr;
    std::vector<int>* layer_heatmap_algo = nullptr;
    std::vector<int>* layer_normalize_mode = nullptr;
    std::vector<float>* layer_heatmap_cell_px = nullptr;
    std::vector<float>* layer_heatmap_bandwidth_px = nullptr;
    std::vector<float>* layer_heatmap_blur_sigma_px = nullptr;
    std::vector<float>* layer_heatmap_percentile_clip = nullptr;
    std::vector<float>* layer_choropleth_gamma = nullptr;
    std::vector<bool>* layer_fill_enabled = nullptr;
    std::vector<bool>* layer_heatmap_zoom_adaptive_bandwidth = nullptr;
    std::vector<bool>* layer_heatmap_multires_enabled = nullptr;
    std::vector<float>* layer_heatmap_multires_blend = nullptr;
    std::vector<bool>* layer_heatmap_use_gradient = nullptr;
    int* active_hover_layer_idx = nullptr;
    int* active_click_layer_idx = nullptr;
    int* parcel_parameter_mode_ptr = nullptr;
    bool* layer_heatmap_state_changed = nullptr;
    bool* layer_fill_state_changed = nullptr;
    bool* layer_hover_state_changed = nullptr;
    bool* layer_inspect_state_changed = nullptr;
    float heatmap_percentile_clip = 0.0f;
    int* heatmap_algo_ptr = nullptr;
    int* heatmap_quality_preset = nullptr;
    float* global_heat_cell_px = nullptr;
    float* heatmap_bandwidth_px = nullptr;
    float* heatmap_blur_sigma_px = nullptr;
    float* global_heatmap_percentile_clip = nullptr;
    bool* heatmap_zoom_adaptive_bandwidth = nullptr;
    bool* heatmap_multires_enabled = nullptr;
    float* heatmap_multires_blend = nullptr;

    std::vector<int>* parcel_vac_notice_by_feature = nullptr;
    std::vector<int>* parcel_vac_rehab_by_feature = nullptr;
    ParcelJurisdictionFilterState* parcel_jurisdiction_filter_state = nullptr;
    const FilterResultSet* owner_text_filter_result_set = nullptr;
    const FilterResultSet* address_text_filter_result_set = nullptr;

    std::unordered_map<std::string, std::string>* owner_class_overrides = nullptr;
    bool* owner_class_overrides_loaded = nullptr;
    bool* owner_class_overrides_dirty = nullptr;
    std::vector<OwnerAggregate>* owner_aggregates = nullptr;
    FilteredAggregateSnapshot* filtered_aggregate_snapshot = nullptr;
    bool* owner_aggregates_dirty = nullptr;
    int* owner_sort_mode = nullptr;
    int* owner_sorted_mode = nullptr;
    int* owner_class_filter_mode = nullptr;
    int* owner_class_assign_mode = nullptr;
    int* selected_owner_anchor = nullptr;
    size_t* owner_cached_parcel_size = nullptr;
    size_t* owner_cached_real_property_size = nullptr;
    int parcel_vacancy_generation_applied = 0;
    int parcel_tax_generation_applied = 0;
    std::atomic<double>* prof_owner_ms_last = nullptr;
    char* owner_search_query = nullptr;
    size_t owner_search_query_size = 0;

    std::string* address_locate_status = nullptr;
    std::vector<AddressLocateMatch>* address_locate_matches = nullptr;
    std::vector<int>* record_year_hist = nullptr;
    std::vector<float>* record_year_hist_plot = nullptr;
    std::vector<size_t>* hist_feature_counts = nullptr;
    std::vector<bool>* hist_enabled = nullptr;
    bool* hist_dirty = nullptr;
    float* record_year_hist_max_bin = nullptr;
    int* record_year_nonzero_min = nullptr;
    int* record_year_nonzero_max = nullptr;
    int* record_year_nonzero_total = nullptr;
    int* selected_record_year = nullptr;
    bool* selected_record_year_dirty = nullptr;
    int* selected_record_year_total = nullptr;
    std::vector<std::string>* selected_record_year_samples = nullptr;

    size_t cached_vac_notice_size = 0;
    size_t cached_vac_rehab_size = 0;
    std::atomic<size_t>* vacant_notice_rows_matched_total = nullptr;
    std::atomic<size_t>* vacant_rehab_rows_matched_total = nullptr;
    std::atomic<size_t>* vacant_parcels_matched_total = nullptr;
    std::atomic<size_t>* vacant_parcels_with_geometry_total = nullptr;

    std::mutex* profile_mutex = nullptr;
    std::vector<ProfileFrameSample>* profile_samples = nullptr;
    size_t* profile_sample_pos = nullptr;
    size_t* profile_sample_count = nullptr;
    std::atomic<bool>* prof_heatmap_gpu_splat_active = nullptr;
    std::atomic<bool>* prof_heatmap_high_quality = nullptr;
    std::atomic<bool>* prof_heatmap_texture_resident = nullptr;
    std::atomic<bool>* prof_heatmap_async_inflight = nullptr;
    std::atomic<size_t>* prof_heatmap_texture_cache_entries = nullptr;
    bool* gpu_profiler_tab_requested = nullptr;
    bool* gpu_profiler_reload_requested = nullptr;
    std::string* current_project_path = nullptr;
    std::string* project_status = nullptr;
    RoadLabelState* road_label_state = nullptr;
    AvCaptureState* av_capture_state = nullptr;
};

void drawRightPanelWindow(const RightPanelContext& ctx);
