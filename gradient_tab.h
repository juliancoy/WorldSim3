#pragma once

#include "types.h"

#include <vector>

struct GradientTabContext {
    const std::vector<LayerDef>* layers = nullptr;
    const std::vector<bool>* layer_heatmap_enabled = nullptr;
    const std::vector<int>* layer_heatmap_max_zoom = nullptr;
    const std::vector<int>* layer_parcel_detail_min_zoom = nullptr;
    const std::vector<int>* layer_heatmap_algo = nullptr;
    const std::vector<float>* layer_heatmap_percentile_clip = nullptr;
    float heatmap_percentile_clip = 99.0f;
    int heatmap_algo = 0;
    int zoom = 0;
};

void drawGradientTab(const GradientTabContext& ctx);
