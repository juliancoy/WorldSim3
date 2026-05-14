#include "render_policy.h"

#include "aggregate_visualization_strategies.h"
#include "worldsim_app_internal.h"

#include <algorithm>

int resolveLayerAggregateAlgo(const HeatmapLayerPolicyContext& ctx, size_t layer_idx) {
    return (ctx.layer_heatmap_algo &&
            layer_idx < ctx.layer_heatmap_algo->size() &&
            (*ctx.layer_heatmap_algo)[layer_idx] >= 0)
        ? (*ctx.layer_heatmap_algo)[layer_idx]
        : ctx.heatmap_algo;
}

bool layerUsesParcelChoroplethDetail(const HeatmapLayerPolicyContext& ctx, size_t layer_idx) {
    if (!ctx.layers || layer_idx >= ctx.layers->size()) return false;
    const auto& layer = (*ctx.layers)[layer_idx];
    const int min_zoom =
        ctx.layer_parcel_detail_min_zoom && layer_idx < ctx.layer_parcel_detail_min_zoom->size()
            ? (*ctx.layer_parcel_detail_min_zoom)[layer_idx]
            : kParcelChoroplethMinZoom;
    return ctx.zoom >= min_zoom &&
           layer.scale == "parcel" &&
           !layer.heatmap_field.empty();
}

bool layerUsesHeatmapAggregate(const HeatmapLayerPolicyContext& ctx, size_t layer_idx) {
    if (!ctx.layers || !ctx.layer_heatmap_enabled || !ctx.layer_heatmap_max_zoom) return false;
    if (layer_idx >= ctx.layer_heatmap_max_zoom->size()) return false;
    if (layer_idx >= ctx.layers->size() ||
        !(*ctx.layers)[layer_idx].enabled ||
        layer_idx >= ctx.layer_heatmap_enabled->size() ||
        !(*ctx.layer_heatmap_enabled)[layer_idx] ||
        ctx.zoom > (*ctx.layer_heatmap_max_zoom)[layer_idx]) {
        return false;
    }
    if (layerUsesParcelChoroplethDetail(ctx, layer_idx)) return false;
    const int aggregate_algo = resolveLayerAggregateAlgo(ctx, layer_idx);
    return aggregate_algo != kAggregateLodGeometry && isHeatmapAggregateMethod(aggregate_algo);
}

bool layerUsesLodGeometry(const HeatmapLayerPolicyContext& ctx, size_t layer_idx) {
    if (!ctx.layers || !ctx.layer_heatmap_enabled || !ctx.layer_heatmap_max_zoom) return false;
    if (layer_idx >= ctx.layers->size() ||
        !(*ctx.layers)[layer_idx].enabled ||
        layer_idx >= ctx.layer_heatmap_enabled->size() ||
        layer_idx >= ctx.layer_heatmap_max_zoom->size() ||
        !(*ctx.layer_heatmap_enabled)[layer_idx] ||
        ctx.zoom > (*ctx.layer_heatmap_max_zoom)[layer_idx]) {
        return false;
    }
    if (layerUsesParcelChoroplethDetail(ctx, layer_idx)) return false;
    return resolveLayerAggregateAlgo(ctx, layer_idx) == kAggregateLodGeometry;
}

void resolveLayerHeatSettings(const HeatmapLayerPolicyContext& ctx, size_t layer_idx, HeatSample& hs) {
    hs.layer = (int)layer_idx;
    hs.algo = resolveLayerAggregateAlgo(ctx, layer_idx);
    hs.cell_px = std::max(2.0f, ctx.layer_heatmap_cell_px && layer_idx < ctx.layer_heatmap_cell_px->size()
        ? (*ctx.layer_heatmap_cell_px)[layer_idx]
        : ctx.global_heat_cell_px);
    hs.bandwidth_px = std::max(1.0f, ctx.layer_heatmap_bandwidth_px && layer_idx < ctx.layer_heatmap_bandwidth_px->size()
        ? (*ctx.layer_heatmap_bandwidth_px)[layer_idx]
        : ctx.heatmap_bandwidth_px);
    hs.blur_sigma_px = std::max(0.0f, ctx.layer_heatmap_blur_sigma_px && layer_idx < ctx.layer_heatmap_blur_sigma_px->size()
        ? (*ctx.layer_heatmap_blur_sigma_px)[layer_idx]
        : ctx.heatmap_blur_sigma_px);
    hs.percentile_clip = std::clamp(
        ctx.layer_heatmap_percentile_clip && layer_idx < ctx.layer_heatmap_percentile_clip->size()
            ? (*ctx.layer_heatmap_percentile_clip)[layer_idx]
            : ctx.heatmap_percentile_clip,
        50.0f,
        100.0f);
    hs.zoom_adaptive_bandwidth =
        ctx.layer_heatmap_zoom_adaptive_bandwidth && layer_idx < ctx.layer_heatmap_zoom_adaptive_bandwidth->size()
            ? (*ctx.layer_heatmap_zoom_adaptive_bandwidth)[layer_idx]
            : ctx.heatmap_zoom_adaptive_bandwidth;
    hs.multires_enabled =
        ctx.layer_heatmap_multires_enabled && layer_idx < ctx.layer_heatmap_multires_enabled->size()
            ? (*ctx.layer_heatmap_multires_enabled)[layer_idx]
            : ctx.heatmap_multires_enabled;
    hs.multires_blend = std::clamp(
        ctx.layer_heatmap_multires_blend && layer_idx < ctx.layer_heatmap_multires_blend->size()
            ? (*ctx.layer_heatmap_multires_blend)[layer_idx]
            : ctx.heatmap_multires_blend,
        0.0f,
        1.0f);
    hs.allow_cpu_fallback = ctx.heatmap_allow_cpu_fallback;
}
