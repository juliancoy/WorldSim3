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

LayerDisplayPolicy resolveLayerDisplayPolicy(const HeatmapLayerPolicyContext& ctx, size_t layer_idx) {
    LayerDisplayPolicy policy;
    policy.aggregate_algo = resolveLayerAggregateAlgo(ctx, layer_idx);
    if (ctx.layer_heatmap_max_zoom && layer_idx < ctx.layer_heatmap_max_zoom->size()) {
        policy.aggregate_max_zoom = (*ctx.layer_heatmap_max_zoom)[layer_idx];
    }
    if (ctx.layer_parcel_detail_min_zoom && layer_idx < ctx.layer_parcel_detail_min_zoom->size()) {
        policy.configured_parcel_detail_min_zoom = (*ctx.layer_parcel_detail_min_zoom)[layer_idx];
        policy.effective_parcel_detail_min_zoom = policy.configured_parcel_detail_min_zoom;
    }
    if (!ctx.layers || layer_idx >= ctx.layers->size()) return policy;

    const auto& layer = (*ctx.layers)[layer_idx];
    policy.value_parcel_layer = layer.scale == "parcel" && !layer.heatmap_field.empty();
    policy.aggregate_configured =
        ctx.layer_heatmap_enabled &&
        ctx.layer_heatmap_max_zoom &&
        layer_idx < ctx.layer_heatmap_enabled->size() &&
        layer_idx < ctx.layer_heatmap_max_zoom->size() &&
        (*ctx.layer_heatmap_enabled)[layer_idx] &&
        (isHeatmapAggregateMethod(policy.aggregate_algo) || policy.aggregate_algo == kAggregateLodGeometry);

    if (policy.value_parcel_layer && policy.aggregate_configured) {
        policy.effective_parcel_detail_min_zoom =
            std::min(policy.configured_parcel_detail_min_zoom, policy.aggregate_max_zoom + 1);
    }

    if (policy.value_parcel_layer && ctx.zoom >= policy.effective_parcel_detail_min_zoom) {
        policy.mode = LayerDisplayMode::ParcelChoroplethDetail;
        return policy;
    }

    if (!policy.aggregate_configured ||
        !layer.enabled ||
        ctx.zoom > policy.aggregate_max_zoom) {
        policy.mode = LayerDisplayMode::PerFeature;
        return policy;
    }

    policy.mode = policy.aggregate_algo == kAggregateLodGeometry
        ? LayerDisplayMode::LodGeometry
        : LayerDisplayMode::Aggregate;
    return policy;
}

int resolveLayerParcelDetailMinZoom(const HeatmapLayerPolicyContext& ctx, size_t layer_idx) {
    return resolveLayerDisplayPolicy(ctx, layer_idx).effective_parcel_detail_min_zoom;
}

bool layerUsesParcelChoroplethDetail(const HeatmapLayerPolicyContext& ctx, size_t layer_idx) {
    return resolveLayerDisplayPolicy(ctx, layer_idx).mode == LayerDisplayMode::ParcelChoroplethDetail;
}

bool layerUsesHeatmapAggregate(const HeatmapLayerPolicyContext& ctx, size_t layer_idx) {
    return resolveLayerDisplayPolicy(ctx, layer_idx).mode == LayerDisplayMode::Aggregate;
}

bool layerUsesLodGeometry(const HeatmapLayerPolicyContext& ctx, size_t layer_idx) {
    return resolveLayerDisplayPolicy(ctx, layer_idx).mode == LayerDisplayMode::LodGeometry;
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
