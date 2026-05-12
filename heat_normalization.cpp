#include "heat_normalization.h"

#include "feature_props.h"
#include "map_render_utils.h"

#include <algorithm>
#include <cmath>

namespace {
float percentileRank(const std::vector<float>& sorted_values, float value) {
    if (sorted_values.size() <= 1) return 0.0f;
    const auto it = std::lower_bound(sorted_values.begin(), sorted_values.end(), value);
    const size_t rank = (size_t)std::distance(sorted_values.begin(), it);
    return std::clamp((float)rank / (float)(sorted_values.size() - 1), 0.0f, 1.0f);
}
}

HeatNormalizationState buildHeatNormalizationState(
    const LayerDef& layer,
    size_t layer_idx,
    const std::vector<int>& layer_normalize_mode,
    const std::vector<float>& layer_heatmap_percentile_clip,
    float heatmap_percentile_clip,
    const std::function<bool(size_t, size_t, const LayerDef::FeatureGeom&)>& feature_passes_filters,
    const std::function<std::string(const LayerDef::FeatureGeom&)>& normalization_group_key) {
    HeatNormalizationState state;
    state.heat_min = std::numeric_limits<float>::infinity();
    state.heat_max = -std::numeric_limits<float>::infinity();
    state.normalize_mode =
        layer_idx < layer_normalize_mode.size()
            ? std::clamp(layer_normalize_mode[layer_idx], 0, 2)
            : 0;
    if (layer.heatmap_field.empty()) return state;

    state.heat_values.reserve(layer.features.size());
    for (size_t fi = 0; fi < layer.features.size(); ++fi) {
        const auto& fg = layer.features[fi];
        if (!feature_passes_filters(layer_idx, fi, fg)) continue;
        float v = 0.0f;
        if (!tryGetFeaturePropertyFloat(fg, layer.heatmap_field, v)) continue;
        state.heat_values.push_back(v);
        const std::string group_key = normalization_group_key(fg);
        if (!group_key.empty()) state.heat_values_by_group[group_key].push_back(v);
        state.heat_min = std::min(state.heat_min, v);
        state.heat_max = std::max(state.heat_max, v);
    }
    if (!state.heat_values.empty()) {
        const float clip_pct = std::clamp(
            layer_idx < layer_heatmap_percentile_clip.size()
                ? layer_heatmap_percentile_clip[layer_idx]
                : heatmap_percentile_clip,
            50.0f,
            100.0f);
        std::sort(state.heat_values.begin(), state.heat_values.end());
        state.heat_min = state.heat_values.front();
        if (clip_pct < 100.0f) {
            const size_t max_idx = state.heat_values.size() - 1;
            const size_t kth = (size_t)std::clamp(
                (int)std::floor((clip_pct / 100.0f) * (double)max_idx),
                0,
                (int)max_idx);
            state.heat_max = std::max(state.heat_min, state.heat_values[kth]);
        } else {
            state.heat_max = state.heat_values.back();
        }
    }
    for (auto& kv : state.heat_values_by_group) std::sort(kv.second.begin(), kv.second.end());
    state.heat_range_valid =
        std::isfinite(state.heat_min) && std::isfinite(state.heat_max) && state.heat_max > state.heat_min;
    return state;
}

bool HeatNormalizationState::normalizedValue(
    const LayerDef::FeatureGeom& fg,
    float value,
    const std::function<std::string(const LayerDef::FeatureGeom&)>& normalization_group_key,
    float& out_t) const {
    if (heat_values.empty()) return false;
    if (normalize_mode == 1) {
        out_t = percentileRank(heat_values, value);
        return true;
    }
    if (normalize_mode == 2) {
        const std::string group_key = normalization_group_key(fg);
        auto git = heat_values_by_group.find(group_key);
        if (git != heat_values_by_group.end() && git->second.size() >= 5) {
            out_t = percentileRank(git->second, value);
        } else {
            out_t = percentileRank(heat_values, value);
        }
        return true;
    }
    if (!heat_range_valid) return false;
    out_t = std::clamp((value - heat_min) / (heat_max - heat_min), 0.0f, 1.0f);
    return true;
}
