#include "map_tab.h"

#include "map_overlay_panels.h"
#include "owner_info.h"
#include "worldsim_app.h"

namespace {
struct MapCornerControlState {
    bool hovered = false;
    bool fullscreen_hovered = false;
    bool snapshot_hovered = false;
};

void mapCornerControlPositions(const MapCanvasSession& session, ImVec2& fullscreen_min, ImVec2& camera_min) {
    constexpr float button = 34.0f;
    constexpr float gap = 8.0f;
    camera_min = ImVec2(
        session.origin.x + session.size.x - 12.0f - button,
        session.origin.y + session.size.y - 12.0f - button);
    fullscreen_min = ImVec2(camera_min.x - gap - button, camera_min.y);
}

void drawMapIconFrame(
    ImDrawList* draw,
    const ImVec2& min,
    const ImVec2& size,
    bool hovered) {
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const ImU32 fill = hovered ? IM_COL32(32, 48, 60, 235) : IM_COL32(17, 24, 32, 215);
    draw->AddRectFilled(min, max, fill, 8.0f);
    draw->AddRect(min, max, hovered ? IM_COL32(125, 220, 255, 180) : IM_COL32(255, 255, 255, 80), 8.0f);
}

void drawMapFullscreenIcon(ImDrawList* draw, const ImVec2& min, bool hovered) {
    constexpr float button = 34.0f;
    drawMapIconFrame(draw, min, ImVec2(button, button), hovered);
    const ImU32 c = IM_COL32(245, 248, 250, 240);
    const float x0 = min.x + 10.0f;
    const float y0 = min.y + 10.0f;
    const float x1 = min.x + button - 10.0f;
    const float y1 = min.y + button - 10.0f;
    const float arm = 6.0f;
    draw->AddLine(ImVec2(x0, y0), ImVec2(x0 + arm, y0), c, 1.8f);
    draw->AddLine(ImVec2(x0, y0), ImVec2(x0, y0 + arm), c, 1.8f);
    draw->AddLine(ImVec2(x1, y0), ImVec2(x1 - arm, y0), c, 1.8f);
    draw->AddLine(ImVec2(x1, y0), ImVec2(x1, y0 + arm), c, 1.8f);
    draw->AddLine(ImVec2(x0, y1), ImVec2(x0 + arm, y1), c, 1.8f);
    draw->AddLine(ImVec2(x0, y1), ImVec2(x0, y1 - arm), c, 1.8f);
    draw->AddLine(ImVec2(x1, y1), ImVec2(x1 - arm, y1), c, 1.8f);
    draw->AddLine(ImVec2(x1, y1), ImVec2(x1, y1 - arm), c, 1.8f);
}

void drawMapCameraIcon(ImDrawList* draw, const ImVec2& min, bool hovered) {
    constexpr float button = 34.0f;
    drawMapIconFrame(draw, min, ImVec2(button, button), hovered);
    const ImU32 c = IM_COL32(245, 248, 250, 240);
    const ImVec2 body_min(min.x + 8.0f, min.y + 12.0f);
    const ImVec2 body_max(min.x + button - 7.0f, min.y + button - 9.0f);
    draw->AddRect(body_min, body_max, c, 3.0f, 0, 1.8f);
    draw->AddRectFilled(ImVec2(min.x + 12.0f, min.y + 9.0f), ImVec2(min.x + 21.0f, min.y + 13.0f), c, 2.0f);
    draw->AddCircle(ImVec2(min.x + 17.5f, min.y + 20.5f), 4.6f, c, 18, 1.8f);
    draw->AddCircleFilled(ImVec2(min.x + 26.0f, min.y + 15.0f), 1.4f, c, 8);
}

void drawMapFpsOverlay(const MapCanvasSession& session) {
    if (!session.draw) return;
    const float fps = ImGui::GetIO().Framerate;
    char label[32];
    std::snprintf(label, sizeof(label), "FPS %.1f", fps);

    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 pad(10.0f, 6.0f);
    const ImVec2 min(session.origin.x + 12.0f, session.origin.y + 12.0f);
    const ImVec2 max(min.x + text_size.x + pad.x * 2.0f, min.y + text_size.y + pad.y * 2.0f);
    session.draw->AddRectFilled(min, max, IM_COL32(17, 24, 32, 215), 8.0f);
    session.draw->AddRect(min, max, IM_COL32(255, 255, 255, 80), 8.0f);
    session.draw->AddText(ImVec2(min.x + pad.x, min.y + pad.y), IM_COL32(245, 248, 250, 240), label);
}

MapCornerControlState hitTestMapCornerControls(const MapTabContext& ctx, const MapCanvasSession& session) {
    MapCornerControlState state;
    constexpr float button = 34.0f;
    ImVec2 fullscreen_min;
    ImVec2 camera_min;
    mapCornerControlPositions(session, fullscreen_min, camera_min);

    ImGui::SetCursorScreenPos(fullscreen_min);
    ImGui::InvisibleButton("##map_fullscreen", ImVec2(button, button));
    state.fullscreen_hovered = ImGui::IsItemHovered();
    if (state.fullscreen_hovered) ImGui::SetTooltip("%s", ctx.map_fullscreen ? "Exit fullscreen" : "Fullscreen map");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && ctx.toggle_map_fullscreen) ctx.toggle_map_fullscreen();

    ImGui::SetCursorScreenPos(camera_min);
    ImGui::InvisibleButton("##map_snapshot", ImVec2(button, button));
    state.snapshot_hovered = ImGui::IsItemHovered();
    if (state.snapshot_hovered) ImGui::SetTooltip("Snapshot");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && ctx.request_snapshot) ctx.request_snapshot();

    state.hovered = state.fullscreen_hovered || state.snapshot_hovered;
    return state;
}

void drawMapCornerControlsVisual(const MapTabContext& ctx, const MapCanvasSession& session, const MapCornerControlState& state) {
    if (!session.draw) return;
    ImVec2 fullscreen_min;
    ImVec2 camera_min;
    mapCornerControlPositions(session, fullscreen_min, camera_min);
    drawMapFullscreenIcon(session.draw, fullscreen_min, state.fullscreen_hovered);
    drawMapCameraIcon(session.draw, camera_min, state.snapshot_hovered);
}
}

void drawMapTabWindow(const MapTabContext& ctx) {
    if (!ctx.root || !ctx.app_settings || !ctx.duckdb_analytics || !ctx.center_lon || !ctx.center_lat || !ctx.zoom ||
        !ctx.layers || !ctx.layer_spatial || !ctx.layer_fallback_scan_cursor || !ctx.map_filter_state || !ctx.query_layers || !ctx.real_property_by_blocklot ||
        !ctx.zoning_metadata || !ctx.zoning_zone_enabled || !ctx.zoning_zone_color || !ctx.parcel_selection ||
        !ctx.selected_parcel_indices || !ctx.show_selected_zone_details || !ctx.selected_zone_idx || !ctx.element_info_state ||
        !ctx.layer_fill_enabled || !ctx.layer_hover_enabled || !ctx.layer_inspect_enabled || !ctx.layer_heatmap_enabled ||
        !ctx.layer_heatmap_algo || !ctx.layer_heatmap_max_zoom || !ctx.layer_parcel_detail_min_zoom || !ctx.layer_heatmap_cell_px ||
        !ctx.layer_heatmap_bandwidth_px || !ctx.layer_heatmap_blur_sigma_px || !ctx.layer_heatmap_percentile_clip ||
        !ctx.layer_heatmap_zoom_adaptive_bandwidth || !ctx.layer_heatmap_multires_enabled || !ctx.layer_heatmap_multires_blend ||
        !ctx.layer_heatmap_use_gradient || !ctx.layer_choropleth_gamma || !ctx.layer_normalize_mode ||
        !ctx.parcel_jurisdiction_filter_state || !ctx.parcel_vac_notice_by_feature || !ctx.parcel_vac_rehab_by_feature ||
        !ctx.parcel_tax_lien_by_feature || !ctx.parcel_tax_sale_by_feature || !ctx.parcel_tax_lien_amount_by_feature ||
        !ctx.parcel_tax_sale_amount_by_feature || !ctx.unified_parcels || !ctx.heatmap_runtime || !ctx.hover_inspector_enabled ||
        !ctx.lazy_tile_download || !ctx.topo_tiles_available_cached || !ctx.topo_vector_available_cached ||
        !ctx.basemap_source_has_any_files_cached || !ctx.tile_root_dir_cached || !ctx.basemap_availability_last_check ||
        !ctx.prof_tiles_drawn_frame || !ctx.prof_features_considered_frame || !ctx.prof_features_drawn_frame ||
        !ctx.prof_tile_ms_last || !ctx.prof_layer_ms_last || !ctx.prof_heatmap_ms_last || !ctx.prof_heat_samples_last ||
        !ctx.prof_heatmap_gpu_splat_active || !ctx.prof_heatmap_high_quality || !ctx.prof_heatmap_cache_valid ||
        !ctx.prof_heatmap_texture_resident || !ctx.prof_heatmap_async_inflight || !ctx.prof_heatmap_cache_key ||
        !ctx.prof_heatmap_texture_cache_entries || !ctx.visible_vacant_parcels_last_frame || !ctx.prof_overlay_ms_last ||
        !ctx.render_fill_attempts_last_frame || !ctx.render_fill_success_last_frame || !ctx.render_fill_no_triangles_last_frame ||
        !ctx.render_fill_bad_indices_last_frame || !ctx.time_cube_service || !ctx.time_cube_ui_result || !ctx.time_cube_ui_loaded ||
        !ctx.time_cube_ui_status || !ctx.time_cube_ui_mutex || !ctx.time_cube_ui_worker || !ctx.time_cube_ui_running ||
        !ctx.time_cube_ui_done || !ctx.time_cube_selected || !ctx.time_cube_year_min || !ctx.time_cube_year_max ||
        !ctx.time_cube_normalize_mode || !ctx.time_cube_show_excluded || !ctx.policy_hierarchy || !ctx.policy_hierarchy_error ||
        !ctx.policy_hierarchy_query || !ctx.policy_hierarchy_scope || !ctx.public_servant_roster || !ctx.people_pay_cached_query ||
        !ctx.people_pay_cached_scope || !ctx.people_pay_cache_matched_count || !ctx.people_pay_visible_rows ||
        !ctx.people_pay_cache_rebuilds || !ctx.people_pay_rendered_rows_last || !ctx.policy_viz_root ||
        !ctx.policy_viz_cached_query || !ctx.policy_viz_cached_scope || !ctx.policy_viz_cached_metric ||
        !ctx.policy_viz_metric || !ctx.policy_viz_cache_rebuilds || !ctx.policy_viz_node_count) {
        return;
    }

    clearParcelGpuDrawState();

    ImGui::SetNextWindowPos(ImVec2(ctx.map_x, ctx.layout_margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ctx.map_w, ctx.main_panel_h), ImGuiCond_Always);
    ImGui::Begin("Map", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (ImGui::BeginTabBar("main_view_tabs")) {
        if (ImGui::BeginTabItem("Map")) {
            MapCanvasSession map_canvas_session = beginMapCanvasSession(MapCanvasSessionContext{
                ctx.center_lon,
                ctx.center_lat,
                ctx.zoom,
                ctx.min_zoom,
                ctx.max_zoom,
                ctx.max_internal_math_zoom,
                ctx.root,
                ctx.app_settings,
                ctx.lazy_tile_download,
                ctx.topo_tiles_available_cached,
                ctx.topo_vector_available_cached,
                ctx.basemap_source_has_any_files_cached,
                ctx.tile_root_dir_cached,
                ctx.basemap_availability_last_check,
                ctx.max_native_tile_zoom,
                ctx.layers,
                ctx.layer_spatial,
                ctx.layer_hover_enabled,
                ctx.layer_inspect_enabled,
                ctx.hover_inspector_mode,
                ctx.hover_inspector_enabled,
                ctx.parcel_layer_idx,
                ctx.zoning_layer_idx,
                ctx.vacant_notice_layer_idx,
                ctx.vacant_rehab_layer_idx,
                ctx.tax_lien_layer_idx,
                ctx.tax_sale_layer_idx,
                ctx.prof_tiles_drawn_frame,
                ctx.prof_tile_ms_last,
                ctx.persistent_projection_cache,
                ctx.persistent_projection_generation,
                ctx.projection_generation,
                ctx.prof_projection_world_ring_cache_entries,
                ctx.prof_projection_world_extent_cache_entries,
                ctx.prof_projection_cache_generation
            });
            const MapCornerControlState map_corner_controls = hitTestMapCornerControls(ctx, map_canvas_session);
            drawMapFpsOverlay(map_canvas_session);

            if (ctx.parcel_layer_idx >= 0 &&
                (size_t)ctx.parcel_layer_idx < ctx.layers->size() &&
                (*ctx.layers)[(size_t)ctx.parcel_layer_idx].enabled) {
                const ImGuiIO& io = ImGui::GetIO();
                const ImVec2 fb_scale = io.DisplayFramebufferScale;
                ParcelGpuDrawConfig parcel_draw_cfg;
                parcel_draw_cfg.active = true;
                parcel_draw_cfg.math_zoom = map_canvas_session.math_zoom;
                parcel_draw_cfg.zoom_scale = (float)(map_canvas_session.zoom_scale * std::max(1.0f, fb_scale.x));
                parcel_draw_cfg.center_world = map_canvas_session.center_world;
                parcel_draw_cfg.viewport_origin =
                    ImVec2(map_canvas_session.origin.x * fb_scale.x, map_canvas_session.origin.y * fb_scale.y);
                parcel_draw_cfg.viewport_size =
                    ImVec2(map_canvas_session.size.x * fb_scale.x, map_canvas_session.size.y * fb_scale.y);
                parcel_draw_cfg.framebuffer_size =
                    ImVec2(io.DisplaySize.x * fb_scale.x, io.DisplaySize.y * fb_scale.y);
                parcel_draw_cfg.view_min_lon = map_canvas_session.view_min_lon;
                parcel_draw_cfg.view_min_lat = map_canvas_session.view_min_lat;
                parcel_draw_cfg.view_max_lon = map_canvas_session.view_max_lon;
                parcel_draw_cfg.view_max_lat = map_canvas_session.view_max_lat;
                configureParcelGpuDrawState(parcel_draw_cfg);
            }

            MapFrameSessionContext map_frame_session_ctx;
            map_frame_session_ctx.root = ctx.root;
            map_frame_session_ctx.duckdb_analytics = ctx.duckdb_analytics;
            map_frame_session_ctx.draw = map_canvas_session.draw;
            map_frame_session_ctx.origin = map_canvas_session.origin;
            map_frame_session_ctx.size = map_canvas_session.size;
            map_frame_session_ctx.zoom = *ctx.zoom;
            map_frame_session_ctx.math_zoom = map_canvas_session.math_zoom;
            map_frame_session_ctx.zoom_scale = map_canvas_session.zoom_scale;
            map_frame_session_ctx.lod_ring_step = map_canvas_session.lod_ring_step;
            map_frame_session_ctx.real_property_layer_idx = ctx.real_property_layer_idx;
            map_frame_session_ctx.parcel_layer_idx = ctx.parcel_layer_idx;
            map_frame_session_ctx.zoning_layer_idx = ctx.zoning_layer_idx;
            map_frame_session_ctx.crime_nibrs_layer_idx = ctx.crime_nibrs_layer_idx;
            map_frame_session_ctx.vacant_notice_layer_idx = ctx.vacant_notice_layer_idx;
            map_frame_session_ctx.vacant_rehab_layer_idx = ctx.vacant_rehab_layer_idx;
            map_frame_session_ctx.tax_lien_layer_idx = ctx.tax_lien_layer_idx;
            map_frame_session_ctx.tax_sale_layer_idx = ctx.tax_sale_layer_idx;
            map_frame_session_ctx.vacant_notice_enabled = map_canvas_session.vacant_notice_enabled;
            map_frame_session_ctx.vacant_rehab_enabled = map_canvas_session.vacant_rehab_enabled;
            map_frame_session_ctx.tax_lien_enabled = map_canvas_session.tax_lien_enabled;
            map_frame_session_ctx.tax_sale_enabled = map_canvas_session.tax_sale_enabled;
            map_frame_session_ctx.view_min_lon = map_canvas_session.view_min_lon;
            map_frame_session_ctx.view_min_lat = map_canvas_session.view_min_lat;
            map_frame_session_ctx.view_max_lon = map_canvas_session.view_max_lon;
            map_frame_session_ctx.view_max_lat = map_canvas_session.view_max_lat;
            map_frame_session_ctx.global_heat_cell_px = ctx.global_heat_cell_px;
            map_frame_session_ctx.heatmap_algo = ctx.heatmap_algo;
            map_frame_session_ctx.heatmap_quality_preset = ctx.heatmap_quality_preset;
            map_frame_session_ctx.heatmap_bandwidth_px = ctx.heatmap_bandwidth_px;
            map_frame_session_ctx.heatmap_blur_sigma_px = ctx.heatmap_blur_sigma_px;
            map_frame_session_ctx.heatmap_percentile_clip = ctx.heatmap_percentile_clip;
            map_frame_session_ctx.heatmap_zoom_adaptive_bandwidth = ctx.heatmap_zoom_adaptive_bandwidth;
            map_frame_session_ctx.heatmap_multires_enabled = ctx.heatmap_multires_enabled;
            map_frame_session_ctx.heatmap_multires_blend = ctx.heatmap_multires_blend;
            map_frame_session_ctx.heatmap_allow_cpu_fallback = ctx.heatmap_allow_cpu_fallback;
            map_frame_session_ctx.heatmap_controls_active = ctx.heatmap_controls_active;
            map_frame_session_ctx.parcel_parameter_mode = ctx.parcel_parameter_mode;
            map_frame_session_ctx.map_polygon_fill_opacity =
                ctx.app_settings ? ctx.app_settings->map_polygon_fill_opacity : 170.0f / 255.0f;
            map_frame_session_ctx.map_hovered = map_canvas_session.map_hovered && !map_corner_controls.hovered;
            map_frame_session_ctx.parcel_hover_active = map_canvas_session.parcel_hover_active;
            map_frame_session_ctx.parcel_inspect_active = map_canvas_session.parcel_inspect_active;
            map_frame_session_ctx.zoning_hover_active = map_canvas_session.zoning_hover_active;
            map_frame_session_ctx.zoning_inspect_active = map_canvas_session.zoning_inspect_active;
            map_frame_session_ctx.vacancy_notice_color = &map_canvas_session.vacancy_notice_color;
            map_frame_session_ctx.vacancy_rehab_color = &map_canvas_session.vacancy_rehab_color;
            map_frame_session_ctx.layers = ctx.layers;
            map_frame_session_ctx.layer_spatial = ctx.layer_spatial;
            map_frame_session_ctx.layer_fallback_scan_cursor = ctx.layer_fallback_scan_cursor;
            map_frame_session_ctx.map_filter_state = ctx.map_filter_state;
            map_frame_session_ctx.query_layers = ctx.query_layers;
            map_frame_session_ctx.real_property_by_blocklot = ctx.real_property_by_blocklot;
            map_frame_session_ctx.parcel_vac_notice_by_feature = ctx.parcel_vac_notice_by_feature;
            map_frame_session_ctx.parcel_vac_rehab_by_feature = ctx.parcel_vac_rehab_by_feature;
            map_frame_session_ctx.parcel_tax_lien_by_feature = ctx.parcel_tax_lien_by_feature;
            map_frame_session_ctx.parcel_tax_sale_by_feature = ctx.parcel_tax_sale_by_feature;
            map_frame_session_ctx.parcel_tax_lien_amount_by_feature = ctx.parcel_tax_lien_amount_by_feature;
            map_frame_session_ctx.parcel_tax_sale_amount_by_feature = ctx.parcel_tax_sale_amount_by_feature;
            map_frame_session_ctx.unified_parcels = ctx.unified_parcels;
            map_frame_session_ctx.selected_parcel_indices = ctx.selected_parcel_indices;
            map_frame_session_ctx.zoning_metadata = ctx.zoning_metadata;
            map_frame_session_ctx.zoning_zone_enabled = ctx.zoning_zone_enabled;
            map_frame_session_ctx.zoning_zone_color = ctx.zoning_zone_color;
            map_frame_session_ctx.parcel_selection = ctx.parcel_selection;
            map_frame_session_ctx.show_selected_zone_details = ctx.show_selected_zone_details;
            map_frame_session_ctx.selected_zone_idx = ctx.selected_zone_idx;
            map_frame_session_ctx.hover_state = &map_canvas_session.hover_state;
            map_frame_session_ctx.layer_fill_enabled = ctx.layer_fill_enabled;
            map_frame_session_ctx.layer_heatmap_enabled = ctx.layer_heatmap_enabled;
            map_frame_session_ctx.layer_heatmap_algo = ctx.layer_heatmap_algo;
            map_frame_session_ctx.layer_heatmap_max_zoom = ctx.layer_heatmap_max_zoom;
            map_frame_session_ctx.layer_parcel_detail_min_zoom = ctx.layer_parcel_detail_min_zoom;
            map_frame_session_ctx.layer_heatmap_cell_px = ctx.layer_heatmap_cell_px;
            map_frame_session_ctx.layer_heatmap_bandwidth_px = ctx.layer_heatmap_bandwidth_px;
            map_frame_session_ctx.layer_heatmap_blur_sigma_px = ctx.layer_heatmap_blur_sigma_px;
            map_frame_session_ctx.layer_heatmap_percentile_clip = ctx.layer_heatmap_percentile_clip;
            map_frame_session_ctx.layer_heatmap_zoom_adaptive_bandwidth = ctx.layer_heatmap_zoom_adaptive_bandwidth;
            map_frame_session_ctx.layer_heatmap_multires_enabled = ctx.layer_heatmap_multires_enabled;
            map_frame_session_ctx.layer_heatmap_multires_blend = ctx.layer_heatmap_multires_blend;
            map_frame_session_ctx.layer_heatmap_use_gradient = ctx.layer_heatmap_use_gradient;
            map_frame_session_ctx.layer_choropleth_gamma = ctx.layer_choropleth_gamma;
            map_frame_session_ctx.layer_normalize_mode = ctx.layer_normalize_mode;
            map_frame_session_ctx.parcel_jurisdiction_option_count = ctx.parcel_jurisdiction_option_count;
            map_frame_session_ctx.parcel_jurisdiction_filter_state = ctx.parcel_jurisdiction_filter_state;
            map_frame_session_ctx.heatmap_runtime = ctx.heatmap_runtime;
            map_frame_session_ctx.projection = map_canvas_session.projection_cache;
            map_frame_session_ctx.should_fill_layer_polygon = map_canvas_session.should_fill_layer_polygon;
            map_frame_session_ctx.project_world = map_canvas_session.project_world;
            map_frame_session_ctx.open_parcel_element = [&](size_t idx) { openElementParcelPage(*ctx.element_info_state, idx); };
            map_frame_session_ctx.real_property_for_parcel = ctx.real_property_for_parcel;
            map_frame_session_ctx.prof_layer_ms_last = ctx.prof_layer_ms_last;
            map_frame_session_ctx.prof_heatmap_ms_last = ctx.prof_heatmap_ms_last;
            map_frame_session_ctx.prof_heat_samples_last = ctx.prof_heat_samples_last;
            map_frame_session_ctx.prof_heatmap_gpu_splat_active = ctx.prof_heatmap_gpu_splat_active;
            map_frame_session_ctx.prof_heatmap_high_quality = ctx.prof_heatmap_high_quality;
            map_frame_session_ctx.prof_heatmap_cache_valid = ctx.prof_heatmap_cache_valid;
            map_frame_session_ctx.prof_heatmap_texture_resident = ctx.prof_heatmap_texture_resident;
            map_frame_session_ctx.prof_heatmap_async_inflight = ctx.prof_heatmap_async_inflight;
            map_frame_session_ctx.prof_heatmap_cache_key = ctx.prof_heatmap_cache_key;
            map_frame_session_ctx.prof_heatmap_texture_cache_entries = ctx.prof_heatmap_texture_cache_entries;
            map_frame_session_ctx.visible_vacant_parcels_last_frame = ctx.visible_vacant_parcels_last_frame;
            map_frame_session_ctx.prof_overlay_ms_last = ctx.prof_overlay_ms_last;
            map_frame_session_ctx.render_fill_attempts_last_frame = ctx.render_fill_attempts_last_frame;
            map_frame_session_ctx.render_fill_success_last_frame = ctx.render_fill_success_last_frame;
            map_frame_session_ctx.render_fill_no_triangles_last_frame = ctx.render_fill_no_triangles_last_frame;
            map_frame_session_ctx.render_fill_bad_indices_last_frame = ctx.render_fill_bad_indices_last_frame;
            map_frame_session_ctx.prof_projection_world_ring_cache_entries = ctx.prof_projection_world_ring_cache_entries;
            map_frame_session_ctx.prof_projection_world_extent_cache_entries = ctx.prof_projection_world_extent_cache_entries;
            map_frame_session_ctx.prof_projection_cache_generation = ctx.prof_projection_cache_generation;
            map_frame_session_ctx.prof_features_considered_frame = ctx.prof_features_considered_frame;
            map_frame_session_ctx.prof_features_drawn_frame = ctx.prof_features_drawn_frame;
            runMapFrameSession(map_frame_session_ctx);
            drawMapCornerControlsVisual(ctx, map_canvas_session, map_corner_controls);

            TimeCubePanelContext time_cube_panel_ctx;
            time_cube_panel_ctx.service = ctx.time_cube_service;
            time_cube_panel_ctx.layers = ctx.layers;
            time_cube_panel_ctx.result = ctx.time_cube_ui_result;
            time_cube_panel_ctx.loaded = ctx.time_cube_ui_loaded;
            time_cube_panel_ctx.status = ctx.time_cube_ui_status;
            time_cube_panel_ctx.mutex = ctx.time_cube_ui_mutex;
            time_cube_panel_ctx.worker = ctx.time_cube_ui_worker;
            time_cube_panel_ctx.running = ctx.time_cube_ui_running;
            time_cube_panel_ctx.done = ctx.time_cube_ui_done;
            time_cube_panel_ctx.selected = ctx.time_cube_selected;
            time_cube_panel_ctx.year_min = ctx.time_cube_year_min;
            time_cube_panel_ctx.year_max = ctx.time_cube_year_max;
            time_cube_panel_ctx.normalize_mode = ctx.time_cube_normalize_mode;
            time_cube_panel_ctx.show_excluded = ctx.time_cube_show_excluded;

            PolicyPanelContext policy_panel_ctx;
            policy_panel_ctx.hierarchy = ctx.policy_hierarchy;
            policy_panel_ctx.hierarchy_loaded = ctx.policy_hierarchy_loaded;
            policy_panel_ctx.hierarchy_error = ctx.policy_hierarchy_error;
            policy_panel_ctx.query = ctx.policy_hierarchy_query;
            policy_panel_ctx.query_capacity = ctx.policy_hierarchy_query_capacity;
            policy_panel_ctx.scope = ctx.policy_hierarchy_scope;
            policy_panel_ctx.roster = ctx.public_servant_roster;
            policy_panel_ctx.people_pay_cached_query = ctx.people_pay_cached_query;
            policy_panel_ctx.people_pay_cached_scope = ctx.people_pay_cached_scope;
            policy_panel_ctx.people_pay_cache_matched_count = ctx.people_pay_cache_matched_count;
            policy_panel_ctx.people_pay_visible_rows = ctx.people_pay_visible_rows;
            policy_panel_ctx.people_pay_cache_rebuilds = ctx.people_pay_cache_rebuilds;
            policy_panel_ctx.people_pay_rendered_rows_last = ctx.people_pay_rendered_rows_last;
            policy_panel_ctx.viz_root = ctx.policy_viz_root;
            policy_panel_ctx.viz_cached_query = ctx.policy_viz_cached_query;
            policy_panel_ctx.viz_cached_scope = ctx.policy_viz_cached_scope;
            policy_panel_ctx.viz_cached_metric = ctx.policy_viz_cached_metric;
            policy_panel_ctx.viz_metric = ctx.policy_viz_metric;
            policy_panel_ctx.viz_cache_rebuilds = ctx.policy_viz_cache_rebuilds;
            policy_panel_ctx.viz_node_count = ctx.policy_viz_node_count;
            drawMapOverlayPanelsPopup(map_canvas_session.origin, map_canvas_session.size, time_cube_panel_ctx, policy_panel_ctx, ctx.layers->size());

            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
