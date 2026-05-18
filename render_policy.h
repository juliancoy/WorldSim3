#pragma once

#include "aggregate_visualization_strategies.h"
#include "heatmap_render.h"
#include "types.h"

#include <vector>

struct HeatmapLayerPolicyContext {
    const std::vector<LayerDef>* layers = nullptr;
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
    int zoom = 0;
    int heatmap_algo = kAggregateNone;
    float global_heat_cell_px = 24.0f;
    float heatmap_bandwidth_px = 18.0f;
    float heatmap_blur_sigma_px = 6.0f;
    float heatmap_percentile_clip = 95.0f;
    bool heatmap_zoom_adaptive_bandwidth = true;
    bool heatmap_multires_enabled = true;
    float heatmap_multires_blend = 0.5f;
    bool heatmap_allow_cpu_fallback = false;
};

enum class LayerDisplayMode {
    PerFeature,
    Aggregate,
    LodGeometry,
    ParcelChoroplethDetail
};

struct LayerDisplayPolicy {
    LayerDisplayMode mode = LayerDisplayMode::PerFeature;
    int aggregate_algo = kAggregateNone;
    int aggregate_max_zoom = 13;
    int configured_parcel_detail_min_zoom = 14;
    int effective_parcel_detail_min_zoom = 14;
    bool aggregate_configured = false;
    bool value_parcel_layer = false;
};

int resolveLayerAggregateAlgo(const HeatmapLayerPolicyContext& ctx, size_t layer_idx);
LayerDisplayPolicy resolveLayerDisplayPolicy(const HeatmapLayerPolicyContext& ctx, size_t layer_idx);
int resolveLayerParcelDetailMinZoom(const HeatmapLayerPolicyContext& ctx, size_t layer_idx);
bool layerUsesParcelChoroplethDetail(const HeatmapLayerPolicyContext& ctx, size_t layer_idx);
bool layerUsesHeatmapAggregate(const HeatmapLayerPolicyContext& ctx, size_t layer_idx);
bool layerUsesLodGeometry(const HeatmapLayerPolicyContext& ctx, size_t layer_idx);
void resolveLayerHeatSettings(const HeatmapLayerPolicyContext& ctx, size_t layer_idx, HeatSample& hs);
