#include "layer_ui_context_builders.h"

LayerUiSharedContext makeLayerUiSharedContext(const LayerUiContextFactoryInput& input) {
    LayerUiSharedContext ctx;
    ctx.root = input.root;
    ctx.layers = input.layers;
    ctx.layer_registry = input.layer_registry;
    ctx.local_layer_exists_cache = input.local_layer_exists_cache;
    ctx.data_freshness_state = input.data_freshness_state;
    ctx.data_freshness_msg = input.data_freshness_msg;
    ctx.data_library_status_msg = input.data_library_status_msg;
    ctx.enqueue_layer_download_request = input.enqueue_layer_download_request;
    ctx.mark_local_layer_exists = input.mark_local_layer_exists;
    ctx.enqueue_hydration = input.enqueue_hydration;
    ctx.heatmap_input_float_enter = input.heatmap_input_float_enter;
    ctx.layer_spatial = input.layer_spatial;
    ctx.layer_states = input.layer_states;
    ctx.status_mutex = input.status_mutex;
    ctx.layer_fill_enabled = input.layer_fill_enabled;
    ctx.layer_hover_enabled = input.layer_hover_enabled;
    ctx.layer_inspect_enabled = input.layer_inspect_enabled;
    ctx.layer_heatmap_enabled = input.layer_heatmap_enabled;
    ctx.layer_heatmap_algo = input.layer_heatmap_algo;
    ctx.layer_heatmap_max_zoom = input.layer_heatmap_max_zoom;
    ctx.layer_parcel_detail_min_zoom = input.layer_parcel_detail_min_zoom;
    ctx.layer_normalize_mode = input.layer_normalize_mode;
    ctx.layer_heatmap_cell_px = input.layer_heatmap_cell_px;
    ctx.layer_heatmap_bandwidth_px = input.layer_heatmap_bandwidth_px;
    ctx.layer_heatmap_blur_sigma_px = input.layer_heatmap_blur_sigma_px;
    ctx.layer_heatmap_percentile_clip = input.layer_heatmap_percentile_clip;
    ctx.layer_heatmap_multires_blend = input.layer_heatmap_multires_blend;
    ctx.layer_heatmap_zoom_adaptive_bandwidth = input.layer_heatmap_zoom_adaptive_bandwidth;
    ctx.layer_heatmap_multires_enabled = input.layer_heatmap_multires_enabled;
    ctx.layer_heatmap_use_gradient = input.layer_heatmap_use_gradient;
    ctx.heatmap_algo = input.heatmap_algo;
    ctx.heatmap_allow_cpu_fallback = input.heatmap_allow_cpu_fallback;
    ctx.layer_fill_mutex = input.layer_fill_mutex;
    ctx.layer_fill_state_changed = input.layer_fill_state_changed;
    ctx.layer_hover_state_changed = input.layer_hover_state_changed;
    ctx.layer_inspect_state_changed = input.layer_inspect_state_changed;
    ctx.layer_heatmap_state_changed = input.layer_heatmap_state_changed;
    ctx.heatmap_controls_active = input.heatmap_controls_active;
    ctx.parcel_parameter_mode = input.parcel_parameter_mode;
    return ctx;
}

LayersPanelUiContext makeLayersPanelUiContext(const LayersPanelContextFactoryInput& input) {
    LayersPanelUiContext ctx;
    ctx.shared = input.shared;
    ctx.parcel_layer_idx = input.parcel_layer_idx;
    ctx.zoom = input.zoom;
    ctx.crime_filter_enabled = input.crime_filter_enabled;
    ctx.crime_filter_use_year = input.crime_filter_use_year;
    ctx.crime_year_min = input.crime_year_min;
    ctx.crime_year_max = input.crime_year_max;
    ctx.crime_filter_homicide = input.crime_filter_homicide;
    ctx.crime_filter_robbery = input.crime_filter_robbery;
    ctx.crime_filter_assault = input.crime_filter_assault;
    ctx.crime_filter_burglary = input.crime_filter_burglary;
    ctx.crime_filter_theft = input.crime_filter_theft;
    ctx.crime_filter_auto_theft = input.crime_filter_auto_theft;
    ctx.crime_filter_drug = input.crime_filter_drug;
    ctx.crime_filter_shooting = input.crime_filter_shooting;
    ctx.crime_nibrs_layer_idx = input.crime_nibrs_layer_idx;
    ctx.crime_legacy_layer_idx = input.crime_legacy_layer_idx;
    ctx.crime_breakdown = input.crime_breakdown;
    ctx.parcel_jurisdiction_filter = input.parcel_jurisdiction_filter;
    ctx.parcel_jurisdiction_filter_dirty = input.parcel_jurisdiction_filter_dirty;
    ctx.parcel_jurisdiction_filter_status = input.parcel_jurisdiction_filter_status;
    return ctx;
}
