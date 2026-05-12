#pragma once

#include "parcel_unified.h"
#include "types.h"

#include <functional>
#include <vector>

struct GradientTabContext {
    const std::vector<LayerDef>* layers = nullptr;
    const std::vector<bool>* layer_heatmap_enabled = nullptr;
    const std::vector<int>* layer_heatmap_max_zoom = nullptr;
    const std::vector<int>* layer_parcel_detail_min_zoom = nullptr;
    const std::vector<int>* layer_heatmap_algo = nullptr;
    const std::vector<float>* layer_heatmap_percentile_clip = nullptr;
    std::vector<float>* layer_choropleth_gamma = nullptr;
    bool* layer_heatmap_state_changed = nullptr;
    int parcel_layer_idx = -1;
    int parcel_parameter_mode = 0;
    const std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;
    std::function<bool(size_t, size_t, const LayerDef::FeatureGeom&)> feature_passes_filters;
    float heatmap_percentile_clip = 99.0f;
    int heatmap_algo = 0;
    int zoom = 0;
};

void drawGradientTab(const GradientTabContext& ctx);
