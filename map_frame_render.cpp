#include "map_frame_render.h"

#include "heatmap_key_builder.h"
#include "profiling.h"
#include "render_layer_pass.h"
#include "render_plan_builder.h"
#include "render_tail_pass.h"

#include <algorithm>

namespace {
double profMsSince(const std::chrono::steady_clock::time_point& begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}
}

void orchestrateMapFrameRender(const RenderFrameOrchestrationContext& ctx) {
    if (!ctx.root || !ctx.layers || !ctx.heatmap_runtime || !ctx.projection) return;

    const float global_heat_cell = std::max(2.0f, ctx.global_heat_cell_px);
    std::vector<HeatSample> heat_samples;

    HeatmapLayerPolicyContext heatmap_policy;
    heatmap_policy.layers = ctx.layers;
    heatmap_policy.layer_heatmap_enabled = ctx.layer_heatmap_enabled;
    heatmap_policy.layer_heatmap_algo = ctx.layer_heatmap_algo;
    heatmap_policy.layer_heatmap_max_zoom = ctx.layer_heatmap_max_zoom;
    heatmap_policy.layer_parcel_detail_min_zoom = ctx.layer_parcel_detail_min_zoom;
    heatmap_policy.layer_heatmap_cell_px = ctx.layer_heatmap_cell_px;
    heatmap_policy.layer_heatmap_bandwidth_px = ctx.layer_heatmap_bandwidth_px;
    heatmap_policy.layer_heatmap_blur_sigma_px = ctx.layer_heatmap_blur_sigma_px;
    heatmap_policy.layer_heatmap_percentile_clip = ctx.layer_heatmap_percentile_clip;
    heatmap_policy.layer_heatmap_zoom_adaptive_bandwidth = ctx.layer_heatmap_zoom_adaptive_bandwidth;
    heatmap_policy.layer_heatmap_multires_enabled = ctx.layer_heatmap_multires_enabled;
    heatmap_policy.layer_heatmap_multires_blend = ctx.layer_heatmap_multires_blend;
    heatmap_policy.zoom = ctx.zoom;
    heatmap_policy.heatmap_algo = ctx.heatmap_algo;
    heatmap_policy.global_heat_cell_px = ctx.global_heat_cell_px;
    heatmap_policy.heatmap_bandwidth_px = ctx.heatmap_bandwidth_px;
    heatmap_policy.heatmap_blur_sigma_px = ctx.heatmap_blur_sigma_px;
    heatmap_policy.heatmap_percentile_clip = ctx.heatmap_percentile_clip;
    heatmap_policy.heatmap_zoom_adaptive_bandwidth = ctx.heatmap_zoom_adaptive_bandwidth;
    heatmap_policy.heatmap_multires_enabled = ctx.heatmap_multires_enabled;
    heatmap_policy.heatmap_multires_blend = ctx.heatmap_multires_blend;
    heatmap_policy.heatmap_allow_cpu_fallback = ctx.heatmap_allow_cpu_fallback;

    bool smooth_only_heatmap = true;
    bool any_active_heatmap = false;
    bool any_active_gpu_splat = false;
    for (size_t i = 0; i < ctx.layers->size(); ++i) {
        if (!layerUsesHeatmapAggregate(heatmap_policy, i)) continue;
        any_active_heatmap = true;
        const int aggregate_algo = resolveLayerAggregateAlgo(heatmap_policy, i);
        if (aggregate_algo == kAggregateGpuSplatBlur) any_active_gpu_splat = true;
        if (!isSmoothHeatmapAggregateMethod(aggregate_algo)) smooth_only_heatmap = false;
    }
    if (!any_active_heatmap) smooth_only_heatmap = false;

    const int requested_heatmap_quality_preset = std::clamp(ctx.heatmap_quality_preset, 0, 2);
    const int effective_heatmap_quality_preset =
        any_active_gpu_splat && !ctx.heatmap_allow_cpu_fallback ? 2 : requested_heatmap_quality_preset;
    const bool high_quality_gpu_aggregate =
        any_active_gpu_splat &&
        !ctx.heatmap_allow_cpu_fallback;
    const int smooth_heat_raster_base_px =
        effective_heatmap_quality_preset == 0 ? 1024 :
        effective_heatmap_quality_preset == 1 ? kSmoothHeatRasterBasePx :
        2048;
    const int smooth_heat_raster_max_px =
        effective_heatmap_quality_preset == 0 ? 1536 :
        effective_heatmap_quality_preset == 1 ? kSmoothHeatRasterMaxPx :
        3072;
    const std::function<bool(size_t)> layer_uses_heatmap_aggregate_fn =
        [&](size_t layer_idx) { return layerUsesHeatmapAggregate(heatmap_policy, layer_idx); };

    HeatmapKeyBuilderContext heatmap_key_ctx;
    heatmap_key_ctx.root = *ctx.root;
    heatmap_key_ctx.layers = ctx.layers;
    heatmap_key_ctx.query_layers = ctx.query_layers;
    heatmap_key_ctx.selected_owners = ctx.selected_owners;
    heatmap_key_ctx.layer_heatmap_use_gradient = ctx.layer_heatmap_use_gradient;
    heatmap_key_ctx.layer_choropleth_gamma = ctx.layer_choropleth_gamma;
    heatmap_key_ctx.layer_heatmap_algo = ctx.layer_heatmap_algo;
    heatmap_key_ctx.layer_normalize_mode = ctx.layer_normalize_mode;
    heatmap_key_ctx.layer_heatmap_max_zoom = ctx.layer_heatmap_max_zoom;
    heatmap_key_ctx.layer_parcel_detail_min_zoom = ctx.layer_parcel_detail_min_zoom;
    heatmap_key_ctx.layer_heatmap_cell_px = ctx.layer_heatmap_cell_px;
    heatmap_key_ctx.layer_heatmap_bandwidth_px = ctx.layer_heatmap_bandwidth_px;
    heatmap_key_ctx.layer_heatmap_blur_sigma_px = ctx.layer_heatmap_blur_sigma_px;
    heatmap_key_ctx.layer_heatmap_percentile_clip = ctx.layer_heatmap_percentile_clip;
    heatmap_key_ctx.layer_heatmap_zoom_adaptive_bandwidth = ctx.layer_heatmap_zoom_adaptive_bandwidth;
    heatmap_key_ctx.layer_heatmap_multires_enabled = ctx.layer_heatmap_multires_enabled;
    heatmap_key_ctx.layer_heatmap_multires_blend = ctx.layer_heatmap_multires_blend;
    heatmap_key_ctx.layer_uses_heatmap_aggregate = &layer_uses_heatmap_aggregate_fn;
    heatmap_key_ctx.zoom = ctx.zoom;
    heatmap_key_ctx.math_zoom = ctx.math_zoom;
    heatmap_key_ctx.heatmap_algo = ctx.heatmap_algo;
    heatmap_key_ctx.effective_heatmap_quality_preset = effective_heatmap_quality_preset;
    heatmap_key_ctx.global_heat_cell_px = ctx.global_heat_cell_px;
    heatmap_key_ctx.heatmap_bandwidth_px = ctx.heatmap_bandwidth_px;
    heatmap_key_ctx.heatmap_blur_sigma_px = ctx.heatmap_blur_sigma_px;
    heatmap_key_ctx.heatmap_percentile_clip = ctx.heatmap_percentile_clip;
    heatmap_key_ctx.heatmap_zoom_adaptive_bandwidth = ctx.heatmap_zoom_adaptive_bandwidth;
    heatmap_key_ctx.heatmap_multires_enabled = ctx.heatmap_multires_enabled;
    heatmap_key_ctx.heatmap_multires_blend = ctx.heatmap_multires_blend;
    heatmap_key_ctx.heatmap_allow_cpu_fallback = ctx.heatmap_allow_cpu_fallback;
    heatmap_key_ctx.filter_enabled = ctx.filter_enabled;
    heatmap_key_ctx.filter_blocklot = ctx.filter_blocklot;
    heatmap_key_ctx.filter_status = ctx.filter_status;
    heatmap_key_ctx.filter_address = ctx.filter_address;
    heatmap_key_ctx.filter_owner = ctx.filter_owner;
    heatmap_key_ctx.filter_zip = ctx.filter_zip;
    heatmap_key_ctx.filter_use_date = ctx.filter_use_date;
    heatmap_key_ctx.filter_year_min = ctx.filter_year_min;
    heatmap_key_ctx.filter_year_max = ctx.filter_year_max;
    heatmap_key_ctx.crime_filter_enabled = ctx.crime_filter_enabled;
    heatmap_key_ctx.crime_filter_use_year = ctx.crime_filter_use_year;
    heatmap_key_ctx.crime_year_min = ctx.crime_year_min;
    heatmap_key_ctx.crime_year_max = ctx.crime_year_max;
    heatmap_key_ctx.crime_filter_homicide = ctx.crime_filter_homicide;
    heatmap_key_ctx.crime_filter_robbery = ctx.crime_filter_robbery;
    heatmap_key_ctx.crime_filter_assault = ctx.crime_filter_assault;
    heatmap_key_ctx.crime_filter_burglary = ctx.crime_filter_burglary;
    heatmap_key_ctx.crime_filter_theft = ctx.crime_filter_theft;
    heatmap_key_ctx.crime_filter_auto_theft = ctx.crime_filter_auto_theft;
    heatmap_key_ctx.crime_filter_drug = ctx.crime_filter_drug;
    heatmap_key_ctx.crime_filter_shooting = ctx.crime_filter_shooting;

    const uint64_t heatmap_view_key = buildHeatmapKey(heatmap_key_ctx, true);
    const uint64_t heatmap_data_key = buildHeatmapKey(heatmap_key_ctx, false);
    const uint64_t heatmap_key = high_quality_gpu_aggregate ? heatmap_data_key : heatmap_view_key;

    const auto heat_prof_begin = std::chrono::steady_clock::now();
    const HeatmapCacheLookup heatmap_cache_lookup = prepareHeatmapAggregateCache(
        *ctx.root,
        *ctx.heatmap_runtime,
        any_active_heatmap,
        smooth_only_heatmap,
        heatmap_key);
    if (ctx.prof_heatmap_ms_last) {
        ctx.prof_heatmap_ms_last->store(profMsSince(heat_prof_begin), std::memory_order_relaxed);
    }
    const CachedAggregateTexture* cached_aggregate_for_key = heatmap_cache_lookup.cached_aggregate_for_key;
    const bool can_use_cached_heatmap = any_active_heatmap && heatmap_cache_lookup.can_use_cached_heatmap;
    const bool aggregate_generation_pending = heatmap_cache_lookup.aggregate_generation_pending;
    const bool should_recompute_heatmap =
        any_active_heatmap &&
        !can_use_cached_heatmap &&
        !aggregate_generation_pending &&
        !ctx.heatmap_controls_active;

    const RawSourceLayerPolicy raw_source_layer_policy{
        ctx.vacant_notice_layer_idx,
        ctx.vacant_rehab_layer_idx,
        ctx.tax_lien_layer_idx,
        ctx.tax_sale_layer_idx
    };

    RenderPlanBuilderContext render_plan_ctx;
    render_plan_ctx.layers = ctx.layers;
    render_plan_ctx.raw_source_layer_policy = raw_source_layer_policy;
    render_plan_ctx.zoning_layer_idx = ctx.zoning_layer_idx;
    render_plan_ctx.vacant_notice_enabled = ctx.vacant_notice_enabled;
    render_plan_ctx.vacant_rehab_enabled = ctx.vacant_rehab_enabled;
    render_plan_ctx.tax_lien_enabled = ctx.tax_lien_enabled;
    render_plan_ctx.tax_sale_enabled = ctx.tax_sale_enabled;
    render_plan_ctx.vacant_notice_layer_idx = ctx.vacant_notice_layer_idx;
    render_plan_ctx.vacant_rehab_layer_idx = ctx.vacant_rehab_layer_idx;
    render_plan_ctx.tax_lien_layer_idx = ctx.tax_lien_layer_idx;
    render_plan_ctx.tax_sale_layer_idx = ctx.tax_sale_layer_idx;
    render_plan_ctx.is_parcel_related_layer = ctx.is_parcel_related_layer;
    render_plan_ctx.layer_uses_heatmap_aggregate = [&](size_t layer_idx) { return layerUsesHeatmapAggregate(heatmap_policy, layer_idx); };
    render_plan_ctx.layer_uses_lod_geometry = [&](size_t layer_idx) { return layerUsesLodGeometry(heatmap_policy, layer_idx); };
    const RenderPlan render_plan = buildRenderPlan(render_plan_ctx);

    RenderLayerPassContext layer_pass_ctx;
    layer_pass_ctx.draw = ctx.draw;
    layer_pass_ctx.origin = ctx.origin;
    layer_pass_ctx.size = ctx.size;
    layer_pass_ctx.center_lon = ctx.center_lon;
    layer_pass_ctx.center_lat = ctx.center_lat;
    layer_pass_ctx.zoom = ctx.zoom_ptr;
    layer_pass_ctx.max_zoom = ctx.max_zoom;
    layer_pass_ctx.math_zoom = ctx.math_zoom;
    layer_pass_ctx.zoom_scale = ctx.zoom_scale;
    layer_pass_ctx.lod_ring_step = ctx.lod_ring_step;
    layer_pass_ctx.parcel_layer_idx = ctx.parcel_layer_idx;
    layer_pass_ctx.zoning_layer_idx = ctx.zoning_layer_idx;
    layer_pass_ctx.vacant_notice_layer_idx = ctx.vacant_notice_layer_idx;
    layer_pass_ctx.vacant_rehab_layer_idx = ctx.vacant_rehab_layer_idx;
    layer_pass_ctx.map_polygon_fill_opacity = ctx.map_polygon_fill_opacity;
    layer_pass_ctx.vacancy_notice_color = ctx.vacancy_notice_color;
    layer_pass_ctx.vacancy_rehab_color = ctx.vacancy_rehab_color;
    layer_pass_ctx.vacant_notice_overlay_enabled = render_plan.overlays.vacant_notice_overlay_enabled;
    layer_pass_ctx.vacant_rehab_overlay_enabled = render_plan.overlays.vacant_rehab_overlay_enabled;
    layer_pass_ctx.view_min_lon = ctx.view_min_lon;
    layer_pass_ctx.view_min_lat = ctx.view_min_lat;
    layer_pass_ctx.view_max_lon = ctx.view_max_lon;
    layer_pass_ctx.view_max_lat = ctx.view_max_lat;
    layer_pass_ctx.should_recompute_heatmap = should_recompute_heatmap;
    layer_pass_ctx.high_quality_gpu_aggregate = high_quality_gpu_aggregate;
    layer_pass_ctx.smooth_only_heatmap = smooth_only_heatmap;
    layer_pass_ctx.can_use_cached_heatmap = can_use_cached_heatmap;
    layer_pass_ctx.heatmap_data_key = heatmap_data_key;
    layer_pass_ctx.layers = ctx.layers;
    layer_pass_ctx.layer_spatial = ctx.layer_spatial;
    layer_pass_ctx.layer_fallback_scan_cursor = ctx.layer_fallback_scan_cursor;
    layer_pass_ctx.layer_fill_enabled = ctx.layer_fill_enabled;
    layer_pass_ctx.layer_heatmap_use_gradient = ctx.layer_heatmap_use_gradient;
    layer_pass_ctx.layer_choropleth_gamma = ctx.layer_choropleth_gamma;
    layer_pass_ctx.layer_normalize_mode = ctx.layer_normalize_mode;
    layer_pass_ctx.layer_heatmap_percentile_clip = ctx.layer_heatmap_percentile_clip;
    layer_pass_ctx.parcel_vac_notice_by_feature = ctx.parcel_vac_notice_by_feature;
    layer_pass_ctx.parcel_vac_rehab_by_feature = ctx.parcel_vac_rehab_by_feature;
    layer_pass_ctx.zoning_zone_enabled = ctx.zoning_zone_enabled;
    layer_pass_ctx.zoning_zone_color = ctx.zoning_zone_color;
    layer_pass_ctx.heatmap_policy = &heatmap_policy;
    layer_pass_ctx.heatmap_runtime = ctx.heatmap_runtime;
    layer_pass_ctx.render_plan = &render_plan;
    layer_pass_ctx.raw_source_layer_policy = raw_source_layer_policy;
    layer_pass_ctx.heat_samples = &heat_samples;
    layer_pass_ctx.projection = ctx.projection;
    layer_pass_ctx.feature_passes_filters = ctx.feature_passes_filters;
    layer_pass_ctx.query_map_color = ctx.query_map_color;
    layer_pass_ctx.should_fill_layer_polygon = ctx.should_fill_layer_polygon;
    layer_pass_ctx.project_world = ctx.project_world;
    layer_pass_ctx.prof_features_considered_frame = ctx.prof_features_considered_frame;
    layer_pass_ctx.prof_features_drawn_frame = ctx.prof_features_drawn_frame;
    runRenderLayerPass(layer_pass_ctx);
    if (ctx.prof_layer_ms_last) {
        ctx.prof_layer_ms_last->store(profMsSince(ctx.layer_prof_begin), std::memory_order_relaxed);
    }
    if (ctx.prof_heat_samples_last) {
        ctx.prof_heat_samples_last->store(heat_samples.size(), std::memory_order_relaxed);
    }

    HeatmapFramePassContext heatmap_frame_ctx;
    heatmap_frame_ctx.root = ctx.root;
    heatmap_frame_ctx.runtime = ctx.heatmap_runtime;
    heatmap_frame_ctx.heat_samples = &heat_samples;
    heatmap_frame_ctx.draw = ctx.draw;
    heatmap_frame_ctx.origin = ctx.origin;
    heatmap_frame_ctx.size = ctx.size;
    heatmap_frame_ctx.view_min_lon = ctx.view_min_lon;
    heatmap_frame_ctx.view_min_lat = ctx.view_min_lat;
    heatmap_frame_ctx.view_max_lon = ctx.view_max_lon;
    heatmap_frame_ctx.view_max_lat = ctx.view_max_lat;
    heatmap_frame_ctx.zoom = ctx.zoom;
    heatmap_frame_ctx.math_zoom = ctx.math_zoom;
    heatmap_frame_ctx.heatmap_algo = ctx.heatmap_algo;
    heatmap_frame_ctx.global_heat_cell = global_heat_cell;
    heatmap_frame_ctx.heatmap_bandwidth_px = ctx.heatmap_bandwidth_px;
    heatmap_frame_ctx.heatmap_blur_sigma_px = ctx.heatmap_blur_sigma_px;
    heatmap_frame_ctx.heatmap_percentile_clip = ctx.heatmap_percentile_clip;
    heatmap_frame_ctx.heatmap_zoom_adaptive_bandwidth = ctx.heatmap_zoom_adaptive_bandwidth;
    heatmap_frame_ctx.heatmap_multires_enabled = ctx.heatmap_multires_enabled;
    heatmap_frame_ctx.heatmap_multires_blend = ctx.heatmap_multires_blend;
    heatmap_frame_ctx.should_recompute_heatmap = should_recompute_heatmap;
    heatmap_frame_ctx.any_active_heatmap = any_active_heatmap;
    heatmap_frame_ctx.any_active_gpu_splat = any_active_gpu_splat;
    heatmap_frame_ctx.smooth_only_heatmap = smooth_only_heatmap;
    heatmap_frame_ctx.can_use_cached_heatmap = can_use_cached_heatmap;
    heatmap_frame_ctx.high_quality_gpu_aggregate = high_quality_gpu_aggregate;
    heatmap_frame_ctx.heatmap_key = heatmap_key;
    heatmap_frame_ctx.smooth_heat_raster_base_px = smooth_heat_raster_base_px;
    heatmap_frame_ctx.smooth_heat_raster_max_px = smooth_heat_raster_max_px;
    heatmap_frame_ctx.cached_aggregate_for_key = cached_aggregate_for_key;
    heatmap_frame_ctx.project_world = ctx.project_world;
    heatmap_frame_ctx.prof_heatmap_gpu_splat_active = ctx.prof_heatmap_gpu_splat_active;
    heatmap_frame_ctx.prof_heatmap_high_quality = ctx.prof_heatmap_high_quality;
    heatmap_frame_ctx.prof_heatmap_cache_valid = ctx.prof_heatmap_cache_valid;
    heatmap_frame_ctx.prof_heatmap_texture_resident = ctx.prof_heatmap_texture_resident;
    heatmap_frame_ctx.prof_heatmap_async_inflight = ctx.prof_heatmap_async_inflight;
    heatmap_frame_ctx.prof_heatmap_cache_key = ctx.prof_heatmap_cache_key;
    heatmap_frame_ctx.prof_heatmap_texture_cache_entries = ctx.prof_heatmap_texture_cache_entries;
    runHeatmapFramePass(heatmap_frame_ctx);

    const auto overlay_prof_begin = std::chrono::steady_clock::now();
    RenderTailPassContext render_tail_ctx;
    render_tail_ctx.draw = ctx.draw;
    render_tail_ctx.origin = ctx.origin;
    render_tail_ctx.size = ctx.size;
    render_tail_ctx.zoom = ctx.zoom;
    render_tail_ctx.math_zoom = ctx.math_zoom;
    render_tail_ctx.parcel_layer_idx = ctx.parcel_layer_idx;
    render_tail_ctx.parcel_parameter_mode = ctx.parcel_parameter_mode;
    render_tail_ctx.parcel_choropleth_gamma =
        ctx.parcel_layer_idx >= 0 && (size_t)ctx.parcel_layer_idx < ctx.layer_choropleth_gamma->size()
            ? (*ctx.layer_choropleth_gamma)[(size_t)ctx.parcel_layer_idx]
            : 1.0f;
    render_tail_ctx.vacant_notice_layer_idx = ctx.vacant_notice_layer_idx;
    render_tail_ctx.vacant_rehab_layer_idx = ctx.vacant_rehab_layer_idx;
    render_tail_ctx.tax_lien_layer_idx = ctx.tax_lien_layer_idx;
    render_tail_ctx.tax_sale_layer_idx = ctx.tax_sale_layer_idx;
    render_tail_ctx.vacant_notice_overlay_enabled = render_plan.overlays.vacant_notice_overlay_enabled;
    render_tail_ctx.vacant_rehab_overlay_enabled = render_plan.overlays.vacant_rehab_overlay_enabled;
    render_tail_ctx.tax_lien_overlay_enabled = render_plan.overlays.tax_lien_overlay_enabled;
    render_tail_ctx.tax_sale_overlay_enabled = render_plan.overlays.tax_sale_overlay_enabled;
    render_tail_ctx.vacancy_notice_color = ctx.vacancy_notice_color;
    render_tail_ctx.vacancy_rehab_color = ctx.vacancy_rehab_color;
    render_tail_ctx.layers = ctx.layers;
    render_tail_ctx.layer_fill_enabled = ctx.layer_fill_enabled;
    render_tail_ctx.parcel_vac_notice_by_feature = ctx.parcel_vac_notice_by_feature;
    render_tail_ctx.parcel_vac_rehab_by_feature = ctx.parcel_vac_rehab_by_feature;
    render_tail_ctx.parcel_tax_lien_by_feature = ctx.parcel_tax_lien_by_feature;
    render_tail_ctx.parcel_tax_sale_by_feature = ctx.parcel_tax_sale_by_feature;
    render_tail_ctx.unified_parcels = ctx.unified_parcels;
    render_tail_ctx.selected_parcel_indices = ctx.selected_parcel_indices;
    render_tail_ctx.projection = ctx.projection;
    render_tail_ctx.feature_passes_filters = ctx.feature_passes_filters;
    render_tail_ctx.should_fill_layer_polygon = ctx.should_fill_layer_polygon;
    render_tail_ctx.project_world = ctx.project_world;
    const RenderTailPassResult render_tail_result = runRenderTailPass(render_tail_ctx);

    if (ctx.visible_vacant_parcels_last_frame) {
        ctx.visible_vacant_parcels_last_frame->store(render_tail_result.visible_vacant_parcels, std::memory_order_relaxed);
    }
    if (ctx.prof_overlay_ms_last) {
        ctx.prof_overlay_ms_last->store(profMsSince(overlay_prof_begin), std::memory_order_relaxed);
    }
    if (ctx.render_fill_attempts_last_frame) {
        ctx.render_fill_attempts_last_frame->store(render_tail_result.fill_stats.attempts, std::memory_order_relaxed);
    }
    if (ctx.render_fill_success_last_frame) {
        ctx.render_fill_success_last_frame->store(render_tail_result.fill_stats.success, std::memory_order_relaxed);
    }
    if (ctx.render_fill_no_triangles_last_frame) {
        ctx.render_fill_no_triangles_last_frame->store(render_tail_result.fill_stats.no_triangles, std::memory_order_relaxed);
    }
    if (ctx.render_fill_bad_indices_last_frame) {
        ctx.render_fill_bad_indices_last_frame->store(render_tail_result.fill_stats.bad_indices, std::memory_order_relaxed);
    }
}
