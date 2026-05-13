#include "left_panel.h"

#include "app_settings.h"
#include "basemap_panel.h"
#include "data_library_panel.h"
#include "dataset_library.h"
#include "imgui.h"
#include "layer_import.h"
#include "layer_ui_context_builders.h"
#include "layers_panel_ui.h"
#include "worldsim_app_internal.h"
#include "zoning_filters_panel.h"

#include <algorithm>
#include <unordered_map>

namespace {
bool heatmapInputFloatEnter(
    bool& heatmap_controls_active,
    const char* label,
    float& value,
    float min_value,
    float max_value,
    const char* format) {
    static std::unordered_map<ImGuiID, float> drafts;
    static std::unordered_map<ImGuiID, bool> was_active;
    const ImGuiID id = ImGui::GetID(label);
    if (!was_active[id]) drafts[id] = value;
    float draft = drafts[id];
    const bool committed = ImGui::InputFloat(label, &draft, 0.0f, 0.0f, format, ImGuiInputTextFlags_EnterReturnsTrue);
    const bool active = ImGui::IsItemActive();
    heatmap_controls_active |= active;
    if (committed) {
        value = std::clamp(draft, min_value, max_value);
        drafts[id] = value;
        was_active[id] = active;
        return true;
    }
    drafts[id] = active ? draft : value;
    was_active[id] = active;
    return false;
}

bool leftPanelContextReady(const LeftPanelContext& ctx) {
    return ctx.root && ctx.app_settings && ctx.layers && ctx.layer_registry &&
        ctx.local_layer_exists_cache && ctx.data_freshness_state && ctx.data_freshness_msg &&
        ctx.data_library_status_msg && ctx.zoom && ctx.center_lon && ctx.center_lat &&
        ctx.hover_inspector_mode && ctx.hover_inspector_enabled && ctx.show_sources_panel &&
        ctx.show_data_library && ctx.parcel_parameter_mode && ctx.layer_spatial &&
        ctx.layer_states && ctx.status_mutex && ctx.layer_fill_enabled &&
        ctx.layer_hover_enabled && ctx.layer_inspect_enabled && ctx.layer_heatmap_enabled &&
        ctx.layer_heatmap_algo && ctx.layer_heatmap_max_zoom && ctx.layer_parcel_detail_min_zoom &&
        ctx.layer_normalize_mode && ctx.layer_heatmap_cell_px && ctx.layer_heatmap_bandwidth_px &&
        ctx.layer_heatmap_blur_sigma_px && ctx.layer_heatmap_percentile_clip &&
        ctx.layer_heatmap_multires_blend && ctx.layer_heatmap_zoom_adaptive_bandwidth &&
        ctx.layer_heatmap_multires_enabled && ctx.layer_heatmap_use_gradient &&
        ctx.heatmap_allow_cpu_fallback && ctx.layer_fill_mutex &&
        ctx.layer_fill_state_changed && ctx.layer_hover_state_changed &&
        ctx.layer_inspect_state_changed && ctx.layer_heatmap_state_changed &&
        ctx.heatmap_controls_active && ctx.crime_filter_enabled && ctx.crime_filter_use_year &&
        ctx.crime_year_min && ctx.crime_year_max && ctx.crime_filter_homicide &&
        ctx.crime_filter_robbery && ctx.crime_filter_assault && ctx.crime_filter_burglary &&
        ctx.crime_filter_theft && ctx.crime_filter_auto_theft && ctx.crime_filter_drug &&
        ctx.crime_filter_shooting && ctx.crime_breakdown && ctx.parcel_jurisdiction_filter &&
        ctx.parcel_jurisdiction_filter_dirty && ctx.parcel_jurisdiction_filter_status &&
        ctx.parcel_jurisdiction_options && ctx.basemap_download && ctx.lazy_tile_download &&
        ctx.basemap_coverage_dirty && ctx.zoning_zone_enabled && ctx.zoning_zone_color &&
        ctx.zoning_zone_label && ctx.zoning_metadata && ctx.zoning_zone_order &&
        ctx.zoning_zone_counts && ctx.zoning_group_zones && ctx.zoning_group_order;
}

void selectAllLayersAndFilters(const LeftPanelContext& ctx) {
    for (auto& layer : *ctx.layers) layer.enabled = true;
    ctx.parcel_jurisdiction_filter->clear();
    for (size_t i = 0; i < ctx.parcel_jurisdiction_option_count; ++i) {
        ctx.parcel_jurisdiction_filter->insert(ctx.parcel_jurisdiction_options[i]);
    }
    *ctx.parcel_jurisdiction_filter_dirty = true;
}

void deselectAllLayersAndFilters(const LeftPanelContext& ctx) {
    for (auto& layer : *ctx.layers) layer.enabled = false;
    ctx.parcel_jurisdiction_filter->clear();
    *ctx.parcel_jurisdiction_filter_dirty = true;
}
}

LeftPanelResult drawLeftPanelWindow(const LeftPanelContext& ctx) {
    LeftPanelResult result;
    if (!leftPanelContextReady(ctx)) return result;

    ImGui::SetNextWindowPos(ImVec2(ctx.layout_margin, ctx.layout_margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ctx.left_panel_w, ctx.main_panel_h), ImGuiCond_Always);
    ImGui::Begin("Layers and Controls", nullptr, ImGuiWindowFlags_NoCollapse);
    if (ImGui::Button("Gear")) *ctx.show_sources_panel = !*ctx.show_sources_panel;
    ImGui::SameLine();
    if (ImGui::Button("Library")) *ctx.show_data_library = !*ctx.show_data_library;
    ImGui::SameLine();
    ImGui::Text("Vulkan map + Vulkan UI");

    size_t local_layer_count = 0;
    size_t downloadable_missing_layer_count = 0;
    size_t queueable_missing_layer_count = 0;
    for (size_t i = 0; i < ctx.local_layer_exists_cache->size(); ++i) {
        if ((*ctx.local_layer_exists_cache)[i]) {
            local_layer_count++;
        } else if (i < ctx.layers->size() &&
                   (!(*ctx.layers)[i].source_url.empty() || layerHasImportSource((*ctx.layers)[i]))) {
            downloadable_missing_layer_count++;
            if (!ctx.layer_download_pending || !ctx.layer_download_pending(i)) queueable_missing_layer_count++;
        }
    }

    ImGui::TextDisabled("Local data: %zu/%zu", local_layer_count, ctx.layers->size());
    result.downloadable_missing_layer_count = downloadable_missing_layer_count;
    result.queueable_missing_layer_count = queueable_missing_layer_count;
    ImGui::SliderInt("Zoom", ctx.zoom, ctx.min_zoom, ctx.max_zoom);
    const double lon_min = -77.25;
    const double lon_max = -76.25;
    const double lat_min = 39.05;
    const double lat_max = 39.75;
    ImGui::SliderScalar("Center Lon", ImGuiDataType_Double, ctx.center_lon, &lon_min, &lon_max, "%.6f");
    ImGui::SliderScalar("Center Lat", ImGuiDataType_Double, ctx.center_lat, &lat_min, &lat_max, "%.6f");

    const char* hover_inspector_mode_options[] = {"None", "Parcels", "Zoning", "Parcels + Zoning"};
    if (ImGui::Combo("Hover Inspector", ctx.hover_inspector_mode, hover_inspector_mode_options, IM_ARRAYSIZE(hover_inspector_mode_options))) {
        *ctx.hover_inspector_mode = std::clamp(*ctx.hover_inspector_mode, 0, 3);
        *ctx.hover_inspector_enabled = *ctx.hover_inspector_mode != 0;
    }

    ImGui::SeparatorText("Heatmap");
    ImGui::TextDisabled("Global heatmap controls removed.");
    ImGui::TextDisabled("Use each layer's settings (⚙) to configure aggregate method and heatmap parameters.");

    bool validation_ui = g_EnableValidationLayers;
    if (ImGui::Checkbox("Vulkan Validation (restart required)", &validation_ui)) {
        g_EnableValidationLayers = validation_ui;
        ctx.app_settings->vulkan_validation_enabled = g_EnableValidationLayers;
        saveAppSettings(*ctx.root, *ctx.app_settings);
    }
    ImGui::TextDisabled("Underlying map sources are independent layers below.");

    int reserve_cpu_cores_ui = std::clamp(ctx.app_settings->reserve_cpu_cores, 0, 8);
    if (ImGui::SliderInt("Reserve CPU cores (restart required)", &reserve_cpu_cores_ui, 0, 8)) {
        ctx.app_settings->reserve_cpu_cores = reserve_cpu_cores_ui;
        saveAppSettings(*ctx.root, *ctx.app_settings);
    }

    ImGui::Separator();
    if (ImGui::Button("Show All")) {
        selectAllLayersAndFilters(ctx);
    }
    ImGui::SameLine();
    if (ImGui::Button("Deselect All")) {
        deselectAllLayersAndFilters(ctx);
    }
    ImGui::SameLine();
    if (ImGui::Button("Hide All")) {
        deselectAllLayersAndFilters(ctx);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(queueable_missing_layer_count == 0);
    if (ImGui::Button("Download All") && ctx.queue_all_missing_layer_downloads) {
        ctx.queue_all_missing_layer_downloads();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::Text("Queue all missing layer datasets with source URLs");
        ImGui::TextDisabled("Missing downloadable: %zu", downloadable_missing_layer_count);
        ImGui::TextDisabled("Queueable now: %zu", queueable_missing_layer_count);
        ImGui::EndTooltip();
    }

    BasemapPanelContext basemap_panel_ctx;
    basemap_panel_ctx.root = ctx.root;
    basemap_panel_ctx.app_settings = ctx.app_settings;
    basemap_panel_ctx.basemap_download = ctx.basemap_download;
    basemap_panel_ctx.lazy_tile_download = ctx.lazy_tile_download;
    basemap_panel_ctx.data_library_status_msg = ctx.data_library_status_msg;
    basemap_panel_ctx.basemap_coverage_dirty = ctx.basemap_coverage_dirty;
    basemap_panel_ctx.osm_missing_tiles_cached = ctx.osm_missing_tiles_cached;
    basemap_panel_ctx.osm_total_tiles_cached = ctx.osm_total_tiles_cached;
    basemap_panel_ctx.topo_missing_tiles_cached = ctx.topo_missing_tiles_cached;
    basemap_panel_ctx.topo_total_tiles_cached = ctx.topo_total_tiles_cached;
    basemap_panel_ctx.topo_tiles_available_cached = ctx.topo_tiles_available_cached;
    basemap_panel_ctx.topo_vector_available_cached = ctx.topo_vector_available_cached;
    basemap_panel_ctx.min_zoom = ctx.min_zoom;
    basemap_panel_ctx.max_native_tile_zoom = ctx.max_native_tile_zoom;
    basemap_panel_ctx.max_satellite_native_tile_zoom = ctx.max_satellite_native_tile_zoom;
    drawBasemapPanel(basemap_panel_ctx);

    LayerUiContextFactoryInput layer_ui_input;
    layer_ui_input.root = *ctx.root;
    layer_ui_input.layers = ctx.layers;
    layer_ui_input.layer_registry = ctx.layer_registry;
    layer_ui_input.local_layer_exists_cache = ctx.local_layer_exists_cache;
    layer_ui_input.data_freshness_state = ctx.data_freshness_state;
    layer_ui_input.data_freshness_msg = ctx.data_freshness_msg;
    layer_ui_input.data_library_status_msg = ctx.data_library_status_msg;
    layer_ui_input.parcel_parameter_mode = ctx.parcel_parameter_mode;
    layer_ui_input.enqueue_layer_download_request = ctx.enqueue_layer_download_request;
    layer_ui_input.mark_local_layer_exists = ctx.mark_local_layer_exists;
    layer_ui_input.enqueue_hydration = ctx.enqueue_hydration;
    layer_ui_input.heatmap_input_float_enter = [&](const char* label, float& value, float min_value, float max_value, const char* format) {
        return heatmapInputFloatEnter(*ctx.heatmap_controls_active, label, value, min_value, max_value, format);
    };
    layer_ui_input.layer_spatial = ctx.layer_spatial;
    layer_ui_input.layer_states = ctx.layer_states;
    layer_ui_input.status_mutex = ctx.status_mutex;
    layer_ui_input.layer_fill_enabled = ctx.layer_fill_enabled;
    layer_ui_input.layer_hover_enabled = ctx.layer_hover_enabled;
    layer_ui_input.layer_inspect_enabled = ctx.layer_inspect_enabled;
    layer_ui_input.layer_heatmap_enabled = ctx.layer_heatmap_enabled;
    layer_ui_input.layer_heatmap_algo = ctx.layer_heatmap_algo;
    layer_ui_input.layer_heatmap_max_zoom = ctx.layer_heatmap_max_zoom;
    layer_ui_input.layer_parcel_detail_min_zoom = ctx.layer_parcel_detail_min_zoom;
    layer_ui_input.layer_normalize_mode = ctx.layer_normalize_mode;
    layer_ui_input.layer_heatmap_cell_px = ctx.layer_heatmap_cell_px;
    layer_ui_input.layer_heatmap_bandwidth_px = ctx.layer_heatmap_bandwidth_px;
    layer_ui_input.layer_heatmap_blur_sigma_px = ctx.layer_heatmap_blur_sigma_px;
    layer_ui_input.layer_heatmap_percentile_clip = ctx.layer_heatmap_percentile_clip;
    layer_ui_input.layer_heatmap_multires_blend = ctx.layer_heatmap_multires_blend;
    layer_ui_input.layer_heatmap_zoom_adaptive_bandwidth = ctx.layer_heatmap_zoom_adaptive_bandwidth;
    layer_ui_input.layer_heatmap_multires_enabled = ctx.layer_heatmap_multires_enabled;
    layer_ui_input.layer_heatmap_use_gradient = ctx.layer_heatmap_use_gradient;
    layer_ui_input.heatmap_algo = ctx.heatmap_algo;
    layer_ui_input.heatmap_allow_cpu_fallback = ctx.heatmap_allow_cpu_fallback;
    layer_ui_input.layer_fill_mutex = ctx.layer_fill_mutex;
    layer_ui_input.layer_fill_state_changed = ctx.layer_fill_state_changed;
    layer_ui_input.layer_hover_state_changed = ctx.layer_hover_state_changed;
    layer_ui_input.layer_inspect_state_changed = ctx.layer_inspect_state_changed;
    layer_ui_input.layer_heatmap_state_changed = ctx.layer_heatmap_state_changed;
    layer_ui_input.heatmap_controls_active = ctx.heatmap_controls_active;
    LayerUiSharedContext layer_ui_shared = makeLayerUiSharedContext(layer_ui_input);

    LayersPanelContextFactoryInput layers_panel_input;
    layers_panel_input.shared = &layer_ui_shared;
    layers_panel_input.parcel_layer_idx = ctx.parcel_layer_idx;
    layers_panel_input.zoom = *ctx.zoom;
    layers_panel_input.crime_filter_enabled = ctx.crime_filter_enabled;
    layers_panel_input.crime_filter_use_year = ctx.crime_filter_use_year;
    layers_panel_input.crime_year_min = ctx.crime_year_min;
    layers_panel_input.crime_year_max = ctx.crime_year_max;
    layers_panel_input.crime_filter_homicide = ctx.crime_filter_homicide;
    layers_panel_input.crime_filter_robbery = ctx.crime_filter_robbery;
    layers_panel_input.crime_filter_assault = ctx.crime_filter_assault;
    layers_panel_input.crime_filter_burglary = ctx.crime_filter_burglary;
    layers_panel_input.crime_filter_theft = ctx.crime_filter_theft;
    layers_panel_input.crime_filter_auto_theft = ctx.crime_filter_auto_theft;
    layers_panel_input.crime_filter_drug = ctx.crime_filter_drug;
    layers_panel_input.crime_filter_shooting = ctx.crime_filter_shooting;
    layers_panel_input.crime_nibrs_layer_idx = ctx.crime_nibrs_layer_idx;
    layers_panel_input.crime_legacy_layer_idx = ctx.crime_legacy_layer_idx;
    layers_panel_input.crime_breakdown = ctx.crime_breakdown;
    layers_panel_input.parcel_jurisdiction_filter = ctx.parcel_jurisdiction_filter;
    layers_panel_input.parcel_jurisdiction_filter_dirty = ctx.parcel_jurisdiction_filter_dirty;
    layers_panel_input.parcel_jurisdiction_filter_status = ctx.parcel_jurisdiction_filter_status;
    LayersPanelUiContext layers_panel_ctx = makeLayersPanelUiContext(layers_panel_input);
    drawLayerCategoriesPanel(layers_panel_ctx);

    ZoningFiltersPanelContext zoning_filters_ctx;
    zoning_filters_ctx.zoning_layer_idx = ctx.zoning_layer_idx;
    zoning_filters_ctx.root = ctx.root;
    zoning_filters_ctx.app_settings = ctx.app_settings;
    zoning_filters_ctx.zoning_zone_enabled = ctx.zoning_zone_enabled;
    zoning_filters_ctx.zoning_zone_color = ctx.zoning_zone_color;
    zoning_filters_ctx.zoning_zone_label = ctx.zoning_zone_label;
    zoning_filters_ctx.zoning_metadata = ctx.zoning_metadata;
    zoning_filters_ctx.zoning_zone_order = ctx.zoning_zone_order;
    zoning_filters_ctx.zoning_zone_counts = ctx.zoning_zone_counts;
    zoning_filters_ctx.zoning_group_zones = ctx.zoning_group_zones;
    zoning_filters_ctx.zoning_group_order = ctx.zoning_group_order;
    result.zoning_filters_changed = drawZoningFiltersPanel(zoning_filters_ctx);
    ImGui::End();
    return result;
}
