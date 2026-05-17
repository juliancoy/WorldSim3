#include "map_frame_session.h"

#include "filter_context_builder.h"
#include "map_frame_render.h"
#include "map_inspection.h"

namespace {
void refreshParcelJurisdictionFilter(const MapFrameSessionContext& ctx) {
    if (!ctx.parcel_jurisdiction_filter_state) return;
    ParcelJurisdictionFilterState& state = *ctx.parcel_jurisdiction_filter_state;
    if (!state.dirty) return;

    if (state.selected_jurisdictions.size() == ctx.parcel_jurisdiction_option_count) {
        state.dirty = false;
        state.result_set = FilterResultSet{};
        state.status = "All Maryland parcels";
    } else if (ctx.parcel_layer_idx < 0) {
        state.dirty = false;
        state.result_set = FilterResultSet{};
        state.status = "No active parcel layer";
    } else if (!ctx.duckdb_analytics->status().last_rebuild_ok) {
        state.status = "DuckDB parcel cache is not ready";
    } else {
        state.dirty = false;
        DuckDbQueryResult jurisdiction_query =
            ctx.duckdb_analytics->queryParcelJurisdictions((size_t)ctx.parcel_layer_idx, state.selected_jurisdictions, 32);
        if (jurisdiction_query.ok) {
            state.result_set = std::move(jurisdiction_query.result_set);
            state.status = jurisdiction_query.message;
        } else {
            state.status = jurisdiction_query.message;
        }
    }
}

FeatureFilterContext buildFrameFilterContext(const MapFrameSessionContext& ctx) {
    FeatureFilterContextFactoryInput filter_input;
    filter_input.layers = ctx.layers;
    filter_input.map_filters = ctx.map_filter_state;
    filter_input.result_set =
        ctx.parcel_jurisdiction_filter_state && ctx.parcel_jurisdiction_filter_state->result_set.active
            ? &ctx.parcel_jurisdiction_filter_state->result_set
            : nullptr;
    filter_input.real_property_by_blocklot = ctx.real_property_by_blocklot;
    filter_input.parcel_vac_notice_by_feature = ctx.parcel_vac_notice_by_feature;
    filter_input.parcel_vac_rehab_by_feature = ctx.parcel_vac_rehab_by_feature;
    filter_input.real_property_layer_idx = ctx.real_property_layer_idx;
    filter_input.parcel_layer_idx = ctx.parcel_layer_idx;
    filter_input.crime_nibrs_layer_idx = ctx.crime_nibrs_layer_idx;
    filter_input.query_layers = ctx.query_layers;
    return makeFeatureFilterContext(filter_input);
}

bool queryMapColorU32(
    const FeatureFilterContext& filter_ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureGeom& fg,
    ImU32& out_color) {
    float query_color[4] = {0, 0, 0, 0};
    if (!queryMapColorForFeature(filter_ctx, layer_idx, feature_idx, fg, query_color)) return false;
    out_color = ImGui::ColorConvertFloat4ToU32(ImVec4(query_color[0], query_color[1], query_color[2], query_color[3]));
    return true;
}

RenderFrameOrchestrationContext buildRenderFrameContext(
    const MapFrameSessionContext& ctx,
    const FeatureFilterContext& filter_ctx) {
    RenderFrameOrchestrationContext render_frame_ctx;
    render_frame_ctx.root = ctx.root;
    render_frame_ctx.draw = ctx.draw;
    render_frame_ctx.origin = ctx.origin;
    render_frame_ctx.size = ctx.size;
    render_frame_ctx.zoom = ctx.zoom;
    render_frame_ctx.math_zoom = ctx.math_zoom;
    render_frame_ctx.zoom_scale = ctx.zoom_scale;
    render_frame_ctx.lod_ring_step = ctx.lod_ring_step;
    render_frame_ctx.zoning_layer_idx = ctx.zoning_layer_idx;
    render_frame_ctx.parcel_layer_idx = ctx.parcel_layer_idx;
    render_frame_ctx.vacant_notice_layer_idx = ctx.vacant_notice_layer_idx;
    render_frame_ctx.vacant_rehab_layer_idx = ctx.vacant_rehab_layer_idx;
    render_frame_ctx.tax_lien_layer_idx = ctx.tax_lien_layer_idx;
    render_frame_ctx.tax_sale_layer_idx = ctx.tax_sale_layer_idx;
    render_frame_ctx.vacant_notice_enabled = ctx.vacant_notice_enabled;
    render_frame_ctx.vacant_rehab_enabled = ctx.vacant_rehab_enabled;
    render_frame_ctx.tax_lien_enabled = ctx.tax_lien_enabled;
    render_frame_ctx.tax_sale_enabled = ctx.tax_sale_enabled;
    render_frame_ctx.view_min_lon = ctx.view_min_lon;
    render_frame_ctx.view_min_lat = ctx.view_min_lat;
    render_frame_ctx.view_max_lon = ctx.view_max_lon;
    render_frame_ctx.view_max_lat = ctx.view_max_lat;
    render_frame_ctx.global_heat_cell_px = ctx.global_heat_cell_px;
    render_frame_ctx.heatmap_algo = ctx.heatmap_algo;
    render_frame_ctx.heatmap_quality_preset = ctx.heatmap_quality_preset;
    render_frame_ctx.heatmap_bandwidth_px = ctx.heatmap_bandwidth_px;
    render_frame_ctx.heatmap_blur_sigma_px = ctx.heatmap_blur_sigma_px;
    render_frame_ctx.heatmap_percentile_clip = ctx.heatmap_percentile_clip;
    render_frame_ctx.heatmap_zoom_adaptive_bandwidth = ctx.heatmap_zoom_adaptive_bandwidth;
    render_frame_ctx.heatmap_multires_enabled = ctx.heatmap_multires_enabled;
    render_frame_ctx.heatmap_multires_blend = ctx.heatmap_multires_blend;
    render_frame_ctx.heatmap_allow_cpu_fallback = ctx.heatmap_allow_cpu_fallback;
    render_frame_ctx.heatmap_controls_active = ctx.heatmap_controls_active;
    render_frame_ctx.parcel_parameter_mode = ctx.parcel_parameter_mode;
    render_frame_ctx.map_polygon_fill_opacity = ctx.map_polygon_fill_opacity;
    render_frame_ctx.vacancy_notice_color = ctx.vacancy_notice_color;
    render_frame_ctx.vacancy_rehab_color = ctx.vacancy_rehab_color;
    render_frame_ctx.layers = ctx.layers;
    render_frame_ctx.layer_spatial = ctx.layer_spatial;
    render_frame_ctx.layer_fallback_scan_cursor = ctx.layer_fallback_scan_cursor;
    render_frame_ctx.layer_fill_enabled = ctx.layer_fill_enabled;
    render_frame_ctx.layer_heatmap_enabled = ctx.layer_heatmap_enabled;
    render_frame_ctx.layer_heatmap_algo = ctx.layer_heatmap_algo;
    render_frame_ctx.layer_heatmap_max_zoom = ctx.layer_heatmap_max_zoom;
    render_frame_ctx.layer_parcel_detail_min_zoom = ctx.layer_parcel_detail_min_zoom;
    render_frame_ctx.layer_heatmap_cell_px = ctx.layer_heatmap_cell_px;
    render_frame_ctx.layer_heatmap_bandwidth_px = ctx.layer_heatmap_bandwidth_px;
    render_frame_ctx.layer_heatmap_blur_sigma_px = ctx.layer_heatmap_blur_sigma_px;
    render_frame_ctx.layer_heatmap_percentile_clip = ctx.layer_heatmap_percentile_clip;
    render_frame_ctx.layer_heatmap_zoom_adaptive_bandwidth = ctx.layer_heatmap_zoom_adaptive_bandwidth;
    render_frame_ctx.layer_heatmap_multires_enabled = ctx.layer_heatmap_multires_enabled;
    render_frame_ctx.layer_heatmap_multires_blend = ctx.layer_heatmap_multires_blend;
    render_frame_ctx.layer_heatmap_use_gradient = ctx.layer_heatmap_use_gradient;
    render_frame_ctx.layer_choropleth_gamma = ctx.layer_choropleth_gamma;
    render_frame_ctx.layer_normalize_mode = ctx.layer_normalize_mode;
    render_frame_ctx.parcel_vac_notice_by_feature = ctx.parcel_vac_notice_by_feature;
    render_frame_ctx.parcel_vac_rehab_by_feature = ctx.parcel_vac_rehab_by_feature;
    render_frame_ctx.parcel_tax_lien_by_feature = ctx.parcel_tax_lien_by_feature;
    render_frame_ctx.parcel_tax_sale_by_feature = ctx.parcel_tax_sale_by_feature;
    render_frame_ctx.unified_parcels = ctx.unified_parcels;
    render_frame_ctx.selected_parcel_indices = ctx.selected_parcel_indices;
    render_frame_ctx.zoning_zone_enabled = ctx.zoning_zone_enabled;
    render_frame_ctx.zoning_zone_color = ctx.zoning_zone_color;
    render_frame_ctx.query_layers = ctx.query_layers;
    render_frame_ctx.selected_owners = &ctx.map_filter_state->selected_owners;
    render_frame_ctx.heatmap_runtime = ctx.heatmap_runtime;
    render_frame_ctx.projection = ctx.projection;
    render_frame_ctx.is_parcel_related_layer = [&](size_t layer_idx) { return isParcelRelatedLayer(filter_ctx, layer_idx); };
    render_frame_ctx.feature_passes_filters =
        [&](size_t layer_idx, size_t feature_idx, const LayerDef::FeatureGeom& fg) {
            return featurePassesFilters(filter_ctx, layer_idx, feature_idx, fg);
        };
    render_frame_ctx.query_map_color =
        [&](size_t layer_idx, size_t feature_idx, const LayerDef::FeatureGeom& fg, ImU32& out_color) {
            return queryMapColorU32(filter_ctx, layer_idx, feature_idx, fg, out_color);
        };
    render_frame_ctx.should_fill_layer_polygon = ctx.should_fill_layer_polygon;
    render_frame_ctx.project_world = ctx.project_world;
    render_frame_ctx.layer_prof_begin = std::chrono::steady_clock::now();
    render_frame_ctx.prof_layer_ms_last = ctx.prof_layer_ms_last;
    render_frame_ctx.prof_heatmap_ms_last = ctx.prof_heatmap_ms_last;
    render_frame_ctx.prof_heat_samples_last = ctx.prof_heat_samples_last;
    render_frame_ctx.prof_heatmap_gpu_splat_active = ctx.prof_heatmap_gpu_splat_active;
    render_frame_ctx.prof_heatmap_high_quality = ctx.prof_heatmap_high_quality;
    render_frame_ctx.prof_heatmap_cache_valid = ctx.prof_heatmap_cache_valid;
    render_frame_ctx.prof_heatmap_texture_resident = ctx.prof_heatmap_texture_resident;
    render_frame_ctx.prof_heatmap_async_inflight = ctx.prof_heatmap_async_inflight;
    render_frame_ctx.prof_heatmap_cache_key = ctx.prof_heatmap_cache_key;
    render_frame_ctx.prof_heatmap_texture_cache_entries = ctx.prof_heatmap_texture_cache_entries;
    render_frame_ctx.visible_vacant_parcels_last_frame = ctx.visible_vacant_parcels_last_frame;
    render_frame_ctx.prof_overlay_ms_last = ctx.prof_overlay_ms_last;
    render_frame_ctx.render_fill_attempts_last_frame = ctx.render_fill_attempts_last_frame;
    render_frame_ctx.render_fill_success_last_frame = ctx.render_fill_success_last_frame;
    render_frame_ctx.render_fill_no_triangles_last_frame = ctx.render_fill_no_triangles_last_frame;
    render_frame_ctx.render_fill_bad_indices_last_frame = ctx.render_fill_bad_indices_last_frame;
    render_frame_ctx.prof_features_considered_frame = ctx.prof_features_considered_frame;
    render_frame_ctx.prof_features_drawn_frame = ctx.prof_features_drawn_frame;
    render_frame_ctx.filter_blocklot = ctx.map_filter_state->blocklot;
    render_frame_ctx.filter_status = ctx.map_filter_state->status;
    render_frame_ctx.filter_address = ctx.map_filter_state->address;
    render_frame_ctx.filter_owner = ctx.map_filter_state->owner;
    render_frame_ctx.filter_zip = ctx.map_filter_state->zip;
    render_frame_ctx.filter_enabled = ctx.map_filter_state->enabled;
    render_frame_ctx.filter_use_date = ctx.map_filter_state->use_date;
    render_frame_ctx.filter_year_min = ctx.map_filter_state->year_min;
    render_frame_ctx.filter_year_max = ctx.map_filter_state->year_max;
    render_frame_ctx.crime_filter_enabled = ctx.map_filter_state->crime.enabled;
    render_frame_ctx.crime_filter_use_year = ctx.map_filter_state->crime.use_year;
    render_frame_ctx.crime_year_min = ctx.map_filter_state->crime.year_min;
    render_frame_ctx.crime_year_max = ctx.map_filter_state->crime.year_max;
    render_frame_ctx.crime_filter_homicide = ctx.map_filter_state->crime.homicide;
    render_frame_ctx.crime_filter_robbery = ctx.map_filter_state->crime.robbery;
    render_frame_ctx.crime_filter_assault = ctx.map_filter_state->crime.assault;
    render_frame_ctx.crime_filter_burglary = ctx.map_filter_state->crime.burglary;
    render_frame_ctx.crime_filter_theft = ctx.map_filter_state->crime.theft;
    render_frame_ctx.crime_filter_auto_theft = ctx.map_filter_state->crime.auto_theft;
    render_frame_ctx.crime_filter_drug = ctx.map_filter_state->crime.drug;
    render_frame_ctx.crime_filter_shooting = ctx.map_filter_state->crime.shooting;
    return render_frame_ctx;
}
}

void runMapFrameSession(const MapFrameSessionContext& ctx) {
    if (!ctx.root || !ctx.duckdb_analytics || !ctx.layers || !ctx.layer_spatial || !ctx.map_filter_state ||
        !ctx.query_layers || !ctx.heatmap_runtime || !ctx.projection) {
        return;
    }

    refreshParcelJurisdictionFilter(ctx);
    const FeatureFilterContext filter_ctx = buildFrameFilterContext(ctx);
    orchestrateMapFrameRender(buildRenderFrameContext(ctx, filter_ctx));
    if (ctx.projection) {
        if (ctx.prof_projection_world_ring_cache_entries) {
            ctx.prof_projection_world_ring_cache_entries->store(ctx.projection->cachedWorldRingEntries(), std::memory_order_relaxed);
        }
        if (ctx.prof_projection_world_extent_cache_entries) {
            ctx.prof_projection_world_extent_cache_entries->store(ctx.projection->cachedWorldExtentEntries(), std::memory_order_relaxed);
        }
    }

    handleMapInspection(MapInspectionContext{
        ctx.map_hovered,
        ctx.parcel_hover_active,
        ctx.parcel_inspect_active,
        ctx.zoning_hover_active,
        ctx.zoning_inspect_active,
        ctx.parcel_layer_idx,
        ctx.zoning_layer_idx,
        ctx.layers,
        ctx.layer_spatial,
        ctx.zoning_metadata,
        ctx.parcel_selection,
        ctx.open_parcel_element,
        ctx.show_selected_zone_details,
        ctx.selected_zone_idx,
        ctx.hover_state,
        ctx.parcel_vac_notice_by_feature,
        ctx.parcel_vac_rehab_by_feature,
        ctx.parcel_tax_lien_by_feature,
        ctx.parcel_tax_sale_by_feature,
        ctx.parcel_tax_lien_amount_by_feature,
        ctx.parcel_tax_sale_amount_by_feature,
        ctx.real_property_for_parcel
    });
}
