#pragma once

#include "dataset_library.h"
#include "layer_registry.h"
#include "layer_runtime.h"
#include "types.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct LayerUiSharedContext {
    std::filesystem::path root;
    std::vector<LayerDef>* layers = nullptr;
    LayerRegistry* layer_registry = nullptr;
    std::vector<bool>* local_layer_exists_cache = nullptr;
    std::vector<FreshnessState>* data_freshness_state = nullptr;
    std::vector<std::string>* data_freshness_msg = nullptr;
    std::string* data_library_status_msg = nullptr;

    std::function<bool(size_t)> enqueue_layer_download_request;
    std::function<void(size_t, bool)> mark_local_layer_exists;
    std::function<void(size_t, bool)> enqueue_hydration;
    std::function<bool(const char*, float&, float, float, const char*)> heatmap_input_float_enter;

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

    int* parcel_parameter_mode = nullptr;
};

struct LayersPanelUiContext {
    LayerUiSharedContext* shared = nullptr;
    int parcel_layer_idx = -1;
    int zoom = 0;

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
    int crime_nibrs_layer_idx = -1;
    int crime_legacy_layer_idx = -1;
    std::vector<std::pair<std::string, int>>* crime_breakdown = nullptr;
};
