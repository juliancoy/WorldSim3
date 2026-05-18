#pragma once

#include "app_settings.h"
#include "data_library_panel.h"
#include "download_queue.h"
#include "filters.h"
#include "layer_registry.h"
#include "layer_runtime.h"
#include "tiles.h"
#include "types.h"
#include "zoning.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct LeftPanelResult {
    bool zoning_filters_changed = false;
    size_t downloadable_missing_layer_count = 0;
    size_t queueable_missing_layer_count = 0;
    bool geography_changed = false;
};

struct LeftPanelContext {
    float layout_margin = 0.0f;
    float left_panel_w = 0.0f;
    float main_panel_h = 0.0f;

    const std::filesystem::path* root = nullptr;
    AppSettings* app_settings = nullptr;

    std::vector<LayerDef>* layers = nullptr;
    LayerRegistry* layer_registry = nullptr;
    std::vector<bool>* local_layer_exists_cache = nullptr;
    std::vector<FreshnessState>* data_freshness_state = nullptr;
    std::vector<std::string>* data_freshness_msg = nullptr;
    std::string* data_library_status_msg = nullptr;

    int* zoom = nullptr;
    int min_zoom = 0;
    int max_zoom = 0;
    double* center_lon = nullptr;
    double* center_lat = nullptr;
    MapFilterState* map_filter_state = nullptr;

    int* hover_inspector_mode = nullptr;
    bool* hover_inspector_enabled = nullptr;
    bool* show_sources_panel = nullptr;
    bool* show_data_library = nullptr;

    int* parcel_parameter_mode = nullptr;
    std::vector<LayerSpatialIndex>* layer_spatial = nullptr;
    std::vector<LayerRuntimeState>* layer_states = nullptr;
    std::mutex* status_mutex = nullptr;

    std::vector<bool>* layer_fill_enabled = nullptr;
    std::vector<bool>* layer_hover_enabled = nullptr;
    std::vector<bool>* layer_inspect_enabled = nullptr;
    std::vector<bool>* layer_heatmap_enabled = nullptr;
    std::vector<int>* layer_heatmap_algo = nullptr;
    std::vector<int>* layer_heatmap_max_zoom = nullptr;
    std::vector<int>* layer_parcel_detail_min_zoom = nullptr;
    std::vector<int>* layer_normalize_mode = nullptr;
    std::vector<float>* layer_heatmap_cell_px = nullptr;
    std::vector<float>* layer_heatmap_bandwidth_px = nullptr;
    std::vector<float>* layer_heatmap_blur_sigma_px = nullptr;
    std::vector<float>* layer_heatmap_percentile_clip = nullptr;
    std::vector<float>* layer_heatmap_multires_blend = nullptr;
    std::vector<bool>* layer_heatmap_zoom_adaptive_bandwidth = nullptr;
    std::vector<bool>* layer_heatmap_multires_enabled = nullptr;
    std::vector<bool>* layer_heatmap_use_gradient = nullptr;
    int heatmap_algo = 0;
    bool* heatmap_allow_cpu_fallback = nullptr;
    std::mutex* layer_fill_mutex = nullptr;
    bool* layer_fill_state_changed = nullptr;
    bool* layer_hover_state_changed = nullptr;
    bool* layer_inspect_state_changed = nullptr;
    bool* layer_heatmap_state_changed = nullptr;
    bool* heatmap_controls_active = nullptr;

    int parcel_layer_idx = -1;
    int crime_nibrs_layer_idx = -1;
    int zoning_layer_idx = -1;

    bool* crime_filter_enabled = nullptr;
    bool* crime_filter_use_year = nullptr;
    int* crime_year_min = nullptr;
    int* crime_year_max = nullptr;
    bool* crime_filter_homicide = nullptr;
    bool* crime_filter_robbery = nullptr;
    bool* crime_filter_assault = nullptr;
    bool* crime_filter_burglary = nullptr;
    bool* crime_filter_theft = nullptr;
    bool* crime_filter_auto_theft = nullptr;
    bool* crime_filter_drug = nullptr;
    bool* crime_filter_shooting = nullptr;
    std::vector<std::pair<std::string, int>>* crime_breakdown = nullptr;

    ParcelJurisdictionFilterState* parcel_jurisdiction_filter_state = nullptr;
    const char* const* parcel_jurisdiction_options = nullptr;
    size_t parcel_jurisdiction_option_count = 0;

    DownloadQueueState* basemap_download = nullptr;
    LazyTileDownloadState* lazy_tile_download = nullptr;
    bool* basemap_coverage_dirty = nullptr;
    size_t osm_missing_tiles_cached = 0;
    size_t osm_total_tiles_cached = 0;
    size_t topo_missing_tiles_cached = 0;
    size_t topo_total_tiles_cached = 0;
    bool topo_tiles_available_cached = false;
    bool topo_vector_available_cached = false;
    int max_native_tile_zoom = 0;
    int max_satellite_native_tile_zoom = 0;
    int max_night_satellite_native_tile_zoom = 0;

    std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr;
    std::unordered_map<std::string, ImVec4>* zoning_zone_color = nullptr;
    std::unordered_map<std::string, std::string>* zoning_zone_label = nullptr;
    std::unordered_map<std::string, ZoneMetadata>* zoning_metadata = nullptr;
    std::vector<std::string>* zoning_zone_order = nullptr;
    std::unordered_map<std::string, size_t>* zoning_zone_counts = nullptr;
    std::unordered_map<std::string, std::vector<std::string>>* zoning_group_zones = nullptr;
    std::vector<std::string>* zoning_group_order = nullptr;

    std::function<bool(size_t)> enqueue_layer_download_request;
    std::function<bool(size_t)> layer_download_pending;
    std::function<size_t()> queue_all_missing_layer_downloads;
    std::function<void(size_t, bool)> mark_local_layer_exists;
    std::function<void(size_t, bool)> enqueue_hydration;
};

LeftPanelResult drawLeftPanelWindow(const LeftPanelContext& ctx);
