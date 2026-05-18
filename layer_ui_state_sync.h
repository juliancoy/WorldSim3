#pragma once

#include "layer_runtime.h"
#include "types.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct LayerUiStateSyncResult {
    bool ui_state_changed = false;
    bool vacant_layer_active = false;
};

struct LayerUiStateSyncContext {
    const std::filesystem::path* root = nullptr;
    std::vector<LayerDef>* layers = nullptr;

    int hover_inspector_mode = 0;
    bool hover_inspector_enabled = false;
    int* last_hover_inspector_mode = nullptr;
    std::vector<bool>* last_enabled_state = nullptr;

    bool zoning_filters_changed = false;
    bool layer_fill_state_changed = false;
    bool layer_hover_state_changed = false;
    bool layer_inspect_state_changed = false;
    bool layer_heatmap_state_changed = false;
    bool heatmap_settings_state_changed = false;

    std::vector<bool>* layer_profile_dirty = nullptr;
    std::function<bool(size_t)> layer_data_available;
    std::function<void(size_t, bool)> enqueue_hydration;

    std::vector<LayerRuntimeState>* layer_states = nullptr;
    std::mutex* status_mutex = nullptr;
    int parcel_layer_idx = -1;
    int real_property_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
    int zoning_layer_idx = -1;

    bool filter_enabled = false;
    const std::string* selected_nation_state = nullptr;
    const std::string* selected_state_region = nullptr;
    const char* filter_owner = nullptr;
    const char* filter_address = nullptr;
    const char* filter_zip = nullptr;

    std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr;
    std::vector<bool>* layer_fill_enabled = nullptr;
    std::vector<bool>* layer_hover_enabled = nullptr;
    std::vector<bool>* layer_inspect_enabled = nullptr;
    std::vector<bool>* layer_heatmap_enabled = nullptr;
    std::vector<int>* layer_heatmap_max_zoom = nullptr;
    std::vector<int>* layer_parcel_detail_min_zoom = nullptr;
    std::vector<bool>* layer_heatmap_use_gradient = nullptr;
    std::vector<float>* layer_choropleth_gamma = nullptr;
    std::vector<int>* layer_heatmap_algo = nullptr;
    std::vector<int>* layer_normalize_mode = nullptr;
    std::vector<float>* layer_heatmap_cell_px = nullptr;
    std::vector<float>* layer_heatmap_bandwidth_px = nullptr;
    std::vector<float>* layer_heatmap_blur_sigma_px = nullptr;
    std::vector<float>* layer_heatmap_percentile_clip = nullptr;
    std::vector<bool>* layer_heatmap_zoom_adaptive_bandwidth = nullptr;
    std::vector<bool>* layer_heatmap_multires_enabled = nullptr;
    std::vector<float>* layer_heatmap_multires_blend = nullptr;

    int* heatmap_algo = nullptr;
    int* heatmap_quality_preset = nullptr;
    float* global_heat_cell_px = nullptr;
    float* heatmap_bandwidth_px = nullptr;
    float* heatmap_blur_sigma_px = nullptr;
    float* heatmap_percentile_clip = nullptr;
    bool* heatmap_zoom_adaptive_bandwidth = nullptr;
    bool* heatmap_multires_enabled = nullptr;
    float* heatmap_multires_blend = nullptr;
    bool* heatmap_allow_cpu_fallback = nullptr;

    bool filter_use_date = false;
    int filter_year_min = 0;
    int filter_year_max = 0;
    const char* filter_blocklot = nullptr;
    const char* filter_status = nullptr;
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
    int crime_year_min = 0;
    int crime_year_max = 0;
    const char* owner_search_query = nullptr;
    const std::unordered_set<std::string>* selected_owners = nullptr;
};

LayerUiStateSyncResult syncLayerUiState(const LayerUiStateSyncContext& ctx);
