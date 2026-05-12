#pragma once

#include "types.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct HeatNormalizationState {
    float heat_min = 0.0f;
    float heat_max = 0.0f;
    bool heat_range_valid = false;
    int normalize_mode = 0;
    std::vector<float> heat_values;
    std::unordered_map<std::string, std::vector<float>> heat_values_by_group;

    bool normalizedValue(
        const LayerDef::FeatureGeom& fg,
        float value,
        const std::function<std::string(const LayerDef::FeatureGeom&)>& normalization_group_key,
        float& out_t) const;
};

HeatNormalizationState buildHeatNormalizationState(
    const LayerDef& layer,
    size_t layer_idx,
    const std::vector<int>& layer_normalize_mode,
    const std::vector<float>& layer_heatmap_percentile_clip,
    float heatmap_percentile_clip,
    const std::function<bool(size_t, size_t, const LayerDef::FeatureGeom&)>& feature_passes_filters,
    const std::function<std::string(const LayerDef::FeatureGeom&)>& normalization_group_key);
