#include "layer_ui_state_sync.h"

#include "layer_runtime_coordinator.h"
#include "layer_state_io.h"

LayerUiStateSyncResult syncLayerUiState(const LayerUiStateSyncContext& ctx) {
    LayerUiStateSyncResult result;
    if (!ctx.root || !ctx.layers || !ctx.last_hover_inspector_mode || !ctx.last_enabled_state ||
        !ctx.layer_profile_dirty || !ctx.layer_states || !ctx.status_mutex ||
        !ctx.zoning_zone_enabled || !ctx.layer_fill_enabled || !ctx.layer_hover_enabled ||
        !ctx.layer_inspect_enabled || !ctx.layer_heatmap_enabled || !ctx.layer_heatmap_max_zoom ||
        !ctx.layer_parcel_detail_min_zoom || !ctx.layer_heatmap_use_gradient ||
        !ctx.layer_choropleth_gamma || !ctx.layer_heatmap_algo || !ctx.layer_normalize_mode ||
        !ctx.layer_heatmap_cell_px || !ctx.layer_heatmap_bandwidth_px ||
        !ctx.layer_heatmap_blur_sigma_px || !ctx.layer_heatmap_percentile_clip ||
        !ctx.layer_heatmap_zoom_adaptive_bandwidth || !ctx.layer_heatmap_multires_enabled ||
        !ctx.layer_heatmap_multires_blend || !ctx.heatmap_algo || !ctx.heatmap_quality_preset ||
        !ctx.global_heat_cell_px || !ctx.heatmap_bandwidth_px || !ctx.heatmap_blur_sigma_px ||
        !ctx.heatmap_percentile_clip || !ctx.heatmap_zoom_adaptive_bandwidth ||
        !ctx.heatmap_multires_enabled || !ctx.heatmap_multires_blend ||
        !ctx.heatmap_allow_cpu_fallback || !ctx.filter_blocklot || !ctx.filter_status ||
        !ctx.filter_owner || !ctx.filter_address || !ctx.filter_zip || !ctx.owner_search_query ||
        !ctx.selected_owners) {
        return result;
    }

    result.ui_state_changed =
        (ctx.hover_inspector_mode != *ctx.last_hover_inspector_mode) ||
        ctx.zoning_filters_changed ||
        ctx.layer_fill_state_changed ||
        ctx.layer_hover_state_changed ||
        ctx.layer_inspect_state_changed ||
        ctx.layer_heatmap_state_changed ||
        ctx.heatmap_settings_state_changed;

    std::vector<size_t> newly_enabled;
    if (ctx.last_enabled_state->size() == ctx.layers->size()) {
        for (size_t i = 0; i < ctx.layers->size(); ++i) {
            if ((*ctx.layers)[i].enabled != (*ctx.last_enabled_state)[i]) {
                result.ui_state_changed = true;
                if (i < ctx.layer_profile_dirty->size()) (*ctx.layer_profile_dirty)[i] = true;
                if ((*ctx.layers)[i].enabled && !(*ctx.last_enabled_state)[i]) newly_enabled.push_back(i);
            }
        }
    } else {
        result.ui_state_changed = true;
        for (size_t i = 0; i < ctx.layers->size(); ++i) {
            if ((*ctx.layers)[i].enabled) newly_enabled.push_back(i);
        }
    }

    for (size_t i : newly_enabled) {
        if (ctx.layer_data_available && ctx.layer_data_available(i)) continue;
        if (ctx.enqueue_hydration) ctx.enqueue_hydration(i, false);
    }

    result.vacant_layer_active =
        (ctx.vacant_notice_layer_idx >= 0 && (size_t)ctx.vacant_notice_layer_idx < ctx.layers->size() &&
         (*ctx.layers)[(size_t)ctx.vacant_notice_layer_idx].enabled) ||
        (ctx.vacant_rehab_layer_idx >= 0 && (size_t)ctx.vacant_rehab_layer_idx < ctx.layers->size() &&
         (*ctx.layers)[(size_t)ctx.vacant_rehab_layer_idx].enabled);

    LayerDependencyCoordinatorContext dependency_ctx;
    dependency_ctx.layers = ctx.layers;
    dependency_ctx.layer_states = ctx.layer_states;
    dependency_ctx.status_mutex = ctx.status_mutex;
    dependency_ctx.parcel_layer_idx = ctx.parcel_layer_idx;
    dependency_ctx.real_property_layer_idx = ctx.real_property_layer_idx;
    dependency_ctx.vacant_notice_layer_idx = ctx.vacant_notice_layer_idx;
    dependency_ctx.vacant_rehab_layer_idx = ctx.vacant_rehab_layer_idx;
    dependency_ctx.tax_lien_layer_idx = ctx.tax_lien_layer_idx;
    dependency_ctx.tax_sale_layer_idx = ctx.tax_sale_layer_idx;
    dependency_ctx.filter_enabled = ctx.filter_enabled;
    dependency_ctx.filter_owner = ctx.filter_owner;
    dependency_ctx.filter_address = ctx.filter_address;
    dependency_ctx.filter_zip = ctx.filter_zip;
    dependency_ctx.enqueue_hydration = ctx.enqueue_hydration;
    coordinateLayerHydrationDependencies(dependency_ctx);

    if (!result.ui_state_changed) return result;

    saveLayerUiState(
        *ctx.root,
        *ctx.layers,
        ctx.hover_inspector_enabled,
        &ctx.hover_inspector_mode,
        nullptr,
        ctx.zoning_zone_enabled,
        ctx.layer_fill_enabled,
        ctx.layer_hover_enabled,
        ctx.layer_inspect_enabled,
        ctx.layer_heatmap_enabled,
        ctx.layer_heatmap_max_zoom,
        ctx.layer_parcel_detail_min_zoom,
        ctx.layer_heatmap_use_gradient,
        ctx.layer_choropleth_gamma,
        ctx.layer_heatmap_algo,
        ctx.layer_normalize_mode,
        ctx.layer_heatmap_cell_px,
        ctx.layer_heatmap_bandwidth_px,
        ctx.layer_heatmap_blur_sigma_px,
        ctx.layer_heatmap_percentile_clip,
        ctx.layer_heatmap_zoom_adaptive_bandwidth,
        ctx.layer_heatmap_multires_enabled,
        ctx.layer_heatmap_multires_blend,
        ctx.heatmap_algo,
        ctx.heatmap_quality_preset,
        ctx.global_heat_cell_px,
        ctx.heatmap_bandwidth_px,
        ctx.heatmap_blur_sigma_px,
        ctx.heatmap_percentile_clip,
        ctx.heatmap_zoom_adaptive_bandwidth,
        ctx.heatmap_multires_enabled,
        ctx.heatmap_multires_blend,
        ctx.heatmap_allow_cpu_fallback);

    saveFilterUiState(
        *ctx.root,
        ctx.filter_enabled,
        ctx.filter_use_date,
        ctx.filter_year_min,
        ctx.filter_year_max,
        ctx.filter_blocklot,
        ctx.filter_status,
        ctx.filter_address,
        ctx.filter_owner,
        ctx.filter_zip,
        ctx.crime_filter_enabled,
        ctx.crime_filter_homicide,
        ctx.crime_filter_robbery,
        ctx.crime_filter_assault,
        ctx.crime_filter_burglary,
        ctx.crime_filter_theft,
        ctx.crime_filter_auto_theft,
        ctx.crime_filter_drug,
        ctx.crime_filter_shooting,
        ctx.crime_filter_use_year,
        ctx.crime_year_min,
        ctx.crime_year_max,
        ctx.owner_search_query,
        *ctx.selected_owners);

    ctx.last_enabled_state->clear();
    ctx.last_enabled_state->reserve(ctx.layers->size());
    for (const auto& layer : *ctx.layers) ctx.last_enabled_state->push_back(layer.enabled);
    *ctx.last_hover_inspector_mode = ctx.hover_inspector_mode;
    return result;
}
