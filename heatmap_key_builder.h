#pragma once

#include "filters.h"
#include "types.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <unordered_set>
#include <vector>

struct HeatmapKeyBuilderContext {
    std::filesystem::path root;
    const std::vector<LayerDef>* layers = nullptr;
    const std::vector<QueryMapLayer>* query_layers = nullptr;
    const std::unordered_set<std::string>* selected_owners = nullptr;
    const std::vector<bool>* layer_heatmap_use_gradient = nullptr;
    const std::vector<int>* layer_heatmap_algo = nullptr;
    const std::vector<int>* layer_normalize_mode = nullptr;
    const std::vector<int>* layer_heatmap_max_zoom = nullptr;
    const std::vector<int>* layer_parcel_detail_min_zoom = nullptr;
    const std::vector<float>* layer_heatmap_cell_px = nullptr;
    const std::vector<float>* layer_heatmap_bandwidth_px = nullptr;
    const std::vector<float>* layer_heatmap_blur_sigma_px = nullptr;
    const std::vector<float>* layer_heatmap_percentile_clip = nullptr;
    const std::vector<bool>* layer_heatmap_zoom_adaptive_bandwidth = nullptr;
    const std::vector<bool>* layer_heatmap_multires_enabled = nullptr;
    const std::vector<float>* layer_heatmap_multires_blend = nullptr;
    const std::function<bool(size_t)>* layer_uses_heatmap_aggregate = nullptr;
    int zoom = 0;
    int math_zoom = 0;
    int heatmap_algo = 0;
    int effective_heatmap_quality_preset = 0;
    float global_heat_cell_px = 24.0f;
    float heatmap_bandwidth_px = 18.0f;
    float heatmap_blur_sigma_px = 6.0f;
    float heatmap_percentile_clip = 95.0f;
    bool heatmap_zoom_adaptive_bandwidth = true;
    bool heatmap_multires_enabled = true;
    float heatmap_multires_blend = 0.5f;
    bool heatmap_allow_cpu_fallback = false;
    bool filter_enabled = false;
    const char* filter_blocklot = "";
    const char* filter_status = "";
    const char* filter_address = "";
    const char* filter_owner = "";
    const char* filter_zip = "";
    bool filter_use_date = false;
    int filter_year_min = 0;
    int filter_year_max = 0;
    bool crime_filter_enabled = false;
    bool crime_filter_use_year = false;
    int crime_year_min = 0;
    int crime_year_max = 0;
    bool crime_filter_homicide = false;
    bool crime_filter_robbery = false;
    bool crime_filter_assault = false;
    bool crime_filter_burglary = false;
    bool crime_filter_theft = false;
    bool crime_filter_auto_theft = false;
    bool crime_filter_drug = false;
    bool crime_filter_shooting = false;
};

uint64_t buildHeatmapKey(const HeatmapKeyBuilderContext& ctx, bool include_view_state);
