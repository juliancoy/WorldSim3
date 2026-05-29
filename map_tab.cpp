#include "map_tab.h"

#include "app_utils.h"
#include "map_overlay_panels.h"
#include "owner_info.h"
#include "ui_fonts.h"
#include "worldsim_app.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <string>

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
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (!draw) return;
    const float fps = ImGui::GetIO().Framerate;
    char label[32];
    std::snprintf(label, sizeof(label), "FPS %.1f", fps);

    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 pad(10.0f, 6.0f);
    const ImVec2 min(session.origin.x + 12.0f, session.origin.y + 12.0f);
    const ImVec2 max(min.x + text_size.x + pad.x * 2.0f, min.y + text_size.y + pad.y * 2.0f);
    draw->PushClipRect(session.origin, ImVec2(session.origin.x + session.size.x, session.origin.y + session.size.y), true);
    draw->AddRectFilled(min, max, IM_COL32(17, 24, 32, 215), 8.0f);
    draw->AddRect(min, max, IM_COL32(255, 255, 255, 80), 8.0f);
    draw->AddText(ImVec2(min.x + pad.x, min.y + pad.y), IM_COL32(245, 248, 250, 240), label);
    draw->PopClipRect();
}

std::string trimCopy(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace((unsigned char)value[begin])) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace((unsigned char)value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

std::string toUpperAsciiCopy(std::string value) {
    for (char& c : value) c = (char)std::toupper((unsigned char)c);
    return value;
}

std::string hostFromUrl(const std::string& url) {
    const size_t scheme = url.find("://");
    const size_t host_begin = scheme == std::string::npos ? 0 : scheme + 3;
    if (host_begin >= url.size()) return {};
    size_t host_end = url.find_first_of("/?#", host_begin);
    if (host_end == std::string::npos) host_end = url.size();
    return url.substr(host_begin, host_end - host_begin);
}

std::string titleCaseHostLabel(std::string host) {
    if (host.empty()) return host;
    std::replace(host.begin(), host.end(), '-', ' ');
    std::replace(host.begin(), host.end(), '.', ' ');
    bool new_word = true;
    for (char& c : host) {
        if (std::isspace((unsigned char)c)) {
            new_word = true;
            continue;
        }
        c = new_word ? (char)std::toupper((unsigned char)c) : (char)std::tolower((unsigned char)c);
        new_word = false;
    }
    return host;
}

std::string inferAgencyFromUrl(const std::string& url) {
    std::string host = hostFromUrl(url);
    std::string lower = host;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (lower.find("hud") != std::string::npos) return "Housing and Urban Development";
    if (lower.find("planning.maryland.gov") != std::string::npos || lower.find("mdgeodata.md.gov") != std::string::npos) {
        return "Maryland Department of Planning";
    }
    if (lower.find("opendata.maryland.gov") != std::string::npos) return "Maryland Open Data";
    if (lower.find("baltimorecity.gov") != std::string::npos) return "Baltimore City Open Data";
    if (lower.find("baltimorecountymd.gov") != std::string::npos) return "Baltimore County";
    if (lower.find("howardcountymd.gov") != std::string::npos) return "Howard County";
    return titleCaseHostLabel(host);
}

std::string primaryParcelSourceLabel(const MapTabContext& ctx) {
    if (!ctx.layers || ctx.parcel_layer_idx < 0 || (size_t)ctx.parcel_layer_idx >= ctx.layers->size()) return {};
    const LayerDef& layer = (*ctx.layers)[(size_t)ctx.parcel_layer_idx];
    for (const std::string& url : layer.source_urls) {
        if (const std::string label = inferAgencyFromUrl(url); !label.empty()) return label;
    }
    if (const std::string label = inferAgencyFromUrl(layer.source_url); !label.empty()) return label;
    if (const std::string label = inferAgencyFromUrl(layer.reference_url); !label.empty()) return label;
    if (const std::string label = inferAgencyFromUrl(layer.import_url); !label.empty()) return label;
    return trimCopy(layer.name);
}

void drawMapTitleOverlay(const MapCanvasSession& session, const std::string& title, const std::string& source_label) {
    if (!session.draw) return;
    const std::string clean_title = trimCopy(title);
    const std::string clean_source = trimCopy(source_label);
    if (clean_title.empty() && clean_source.empty()) return;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (!draw) return;

    ImFont* font = getWorldsimMapTitleFont();
    if (!font) font = ImGui::GetFont();
    if (!font) return;
    const float title_font_size = 48.0f;
    const float source_font_size = ImGui::GetFontSize() * 1.0f;
    const ImVec2 title_size =
        clean_title.empty() ? ImVec2(0.0f, 0.0f) : font->CalcTextSizeA(title_font_size, FLT_MAX, 0.0f, clean_title.c_str());
    const std::string source_text = clean_source.empty() ? std::string() : ("Source: " + clean_source);
    const ImVec2 source_size =
        source_text.empty() ? ImVec2(0.0f, 0.0f) : font->CalcTextSizeA(source_font_size, FLT_MAX, 0.0f, source_text.c_str());
    const float content_w = std::max(title_size.x, source_size.x);
    const float content_h = title_size.y + (source_text.empty() ? 0.0f : (6.0f + source_size.y));
    const ImVec2 pad(18.0f, 12.0f);
    const ImVec2 box_min(
        session.origin.x + std::max(0.0f, (session.size.x - content_w) * 0.5f) - pad.x,
        session.origin.y + 14.0f);
    const ImVec2 box_max(box_min.x + content_w + pad.x * 2.0f, box_min.y + content_h + pad.y * 2.0f);

    draw->PushClipRect(session.origin, ImVec2(session.origin.x + session.size.x, session.origin.y + session.size.y), true);
    draw->AddRectFilled(box_min, box_max, IM_COL32(17, 24, 32, 205), 12.0f);
    draw->AddRect(box_min, box_max, IM_COL32(255, 255, 255, 72), 12.0f);

    float y = box_min.y + pad.y;
    if (!clean_title.empty()) {
        const float title_x = session.origin.x + session.size.x * 0.5f - title_size.x * 0.5f;
        draw->AddText(font, title_font_size, ImVec2(title_x + 1.0f, y + 1.0f), IM_COL32(0, 0, 0, 150), clean_title.c_str());
        draw->AddText(font, title_font_size, ImVec2(title_x, y), IM_COL32(250, 250, 250, 245), clean_title.c_str());
        y += title_size.y + 6.0f;
    }
    if (!source_text.empty()) {
        const float source_x = session.origin.x + session.size.x * 0.5f - source_size.x * 0.5f;
        draw->AddText(font, source_font_size, ImVec2(source_x + 1.0f, y + 1.0f), IM_COL32(0, 0, 0, 140), source_text.c_str());
        draw->AddText(font, source_font_size, ImVec2(source_x, y), IM_COL32(220, 226, 232, 235), source_text.c_str());
    }
    draw->PopClipRect();
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

bool isZoningPolygonLayerForGpu(const LayerDef& layer) {
    if (layer.scale == "point") return false;
    if (layer.category == LayerDef::Category::Zoning) return true;
    std::string file_lower = layer.file;
    std::string name_lower = layer.name;
    std::transform(file_lower.begin(), file_lower.end(), file_lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return file_lower.find("zoning") != std::string::npos ||
           name_lower.find("zoning") != std::string::npos;
}

bool isGenericPolygonLayerForGpu(const LayerDef& layer) {
    return !layerUsesPointGeometry(layer) && !layerUsesPolylineGeometry(layer);
}
}

void drawMapTabWindow(const MapTabContext& ctx) {
    if (!ctx.root || !ctx.app_settings || !ctx.duckdb_analytics || !ctx.center_lon || !ctx.center_lat || !ctx.zoom ||
        !ctx.layers || !ctx.layer_spatial || !ctx.map_filter_state || !ctx.query_layers || !ctx.real_property_by_blocklot ||
        !ctx.zoning_metadata || !ctx.zoning_zone_enabled || !ctx.zoning_zone_color || !ctx.parcel_selection ||
        !ctx.selected_parcel_ids || !ctx.show_selected_zone_details || !ctx.selected_zone_idx || !ctx.element_info_state ||
        !ctx.layer_fill_enabled || !ctx.layer_hover_enabled || !ctx.layer_inspect_enabled || !ctx.layer_heatmap_enabled ||
        !ctx.layer_heatmap_algo || !ctx.layer_heatmap_max_zoom || !ctx.layer_parcel_detail_min_zoom || !ctx.layer_heatmap_cell_px ||
        !ctx.layer_heatmap_bandwidth_px || !ctx.layer_heatmap_blur_sigma_px || !ctx.layer_heatmap_percentile_clip ||
        !ctx.layer_heatmap_zoom_adaptive_bandwidth || !ctx.layer_heatmap_multires_enabled || !ctx.layer_heatmap_multires_blend ||
        !ctx.layer_heatmap_use_gradient || !ctx.layer_choropleth_gamma || !ctx.layer_normalize_mode ||
        !ctx.parcel_jurisdiction_filter_state || !ctx.parcel_vac_notice_by_feature || !ctx.parcel_vac_rehab_by_feature ||
        !ctx.parcel_tax_lien_by_feature || !ctx.parcel_tax_sale_by_feature || !ctx.parcel_tax_lien_amount_by_feature ||
        !ctx.parcel_tax_sale_amount_by_feature || !ctx.unified_parcels || !ctx.heatmap_runtime ||
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
    MapCanvasSessionContext map_canvas_ctx{
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
                ctx.point_geometry_artifacts,
                ctx.polygon_geometry_artifacts,
                ctx.parcel_render_blob,
                ctx.layer_spatial,
                ctx.layer_hover_enabled,
                ctx.layer_inspect_enabled,
                ctx.active_hover_layer_idx,
                ctx.active_click_layer_idx,
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
            };
            MapCanvasSession map_canvas_session = beginMapCanvasSession(map_canvas_ctx);
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
                parcel_draw_cfg.center_lonlat = ImVec2(
                    ctx.center_lon ? (float)*ctx.center_lon : 0.0f,
                    ctx.center_lat ? (float)*ctx.center_lat : 0.0f);
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
            bool any_zoning_gpu_layer_enabled = false;
            bool any_point_gpu_layer_enabled = false;
            bool any_polyline_gpu_layer_enabled = false;
            const ImGuiIO& io = ImGui::GetIO();
            const ImVec2 fb_scale = io.DisplayFramebufferScale;
            for (size_t layer_idx = 0; layer_idx < ctx.layers->size(); ++layer_idx) {
                const LayerDef& layer = (*ctx.layers)[layer_idx];
                if (!layer.enabled || !isGenericPolygonLayerForGpu(layer) || (int)layer_idx == ctx.parcel_layer_idx) {
                    clearZoningGpuDrawState(layer_idx);
                } else {
                    any_zoning_gpu_layer_enabled = true;
                    ParcelGpuDrawConfig zoning_draw_cfg;
                    zoning_draw_cfg.active = true;
                    zoning_draw_cfg.math_zoom = map_canvas_session.math_zoom;
                    zoning_draw_cfg.zoom_scale = (float)(map_canvas_session.zoom_scale * std::max(1.0f, fb_scale.x));
                    zoning_draw_cfg.center_lonlat = ImVec2(
                        ctx.center_lon ? (float)*ctx.center_lon : 0.0f,
                        ctx.center_lat ? (float)*ctx.center_lat : 0.0f);
                    zoning_draw_cfg.center_world = map_canvas_session.center_world;
                    zoning_draw_cfg.viewport_origin =
                        ImVec2(map_canvas_session.origin.x * fb_scale.x, map_canvas_session.origin.y * fb_scale.y);
                    zoning_draw_cfg.viewport_size =
                        ImVec2(map_canvas_session.size.x * fb_scale.x, map_canvas_session.size.y * fb_scale.y);
                    zoning_draw_cfg.framebuffer_size =
                        ImVec2(io.DisplaySize.x * fb_scale.x, io.DisplaySize.y * fb_scale.y);
                    zoning_draw_cfg.view_min_lon = map_canvas_session.view_min_lon;
                    zoning_draw_cfg.view_min_lat = map_canvas_session.view_min_lat;
                    zoning_draw_cfg.view_max_lon = map_canvas_session.view_max_lon;
                    zoning_draw_cfg.view_max_lat = map_canvas_session.view_max_lat;
                    configureZoningGpuDrawState(layer_idx, zoning_draw_cfg);
                }

                if (!layer.enabled || !layerUsesPointGeometry(layer) || (int)layer_idx == ctx.crime_nibrs_layer_idx) {
                    clearPointLayerGpuDrawState(layer_idx);
                } else {
                    any_point_gpu_layer_enabled = true;
                    ParcelGpuDrawConfig point_draw_cfg;
                    point_draw_cfg.active = true;
                    point_draw_cfg.math_zoom = map_canvas_session.math_zoom;
                    point_draw_cfg.zoom_scale = (float)(map_canvas_session.zoom_scale * std::max(1.0f, fb_scale.x));
                    point_draw_cfg.center_lonlat = ImVec2(
                        ctx.center_lon ? (float)*ctx.center_lon : 0.0f,
                        ctx.center_lat ? (float)*ctx.center_lat : 0.0f);
                    point_draw_cfg.center_world = map_canvas_session.center_world;
                    point_draw_cfg.viewport_origin =
                        ImVec2(map_canvas_session.origin.x * fb_scale.x, map_canvas_session.origin.y * fb_scale.y);
                    point_draw_cfg.viewport_size =
                        ImVec2(map_canvas_session.size.x * fb_scale.x, map_canvas_session.size.y * fb_scale.y);
                    point_draw_cfg.framebuffer_size =
                        ImVec2(io.DisplaySize.x * fb_scale.x, io.DisplaySize.y * fb_scale.y);
                    point_draw_cfg.view_min_lon = map_canvas_session.view_min_lon;
                    point_draw_cfg.view_min_lat = map_canvas_session.view_min_lat;
                    point_draw_cfg.view_max_lon = map_canvas_session.view_max_lon;
                    point_draw_cfg.view_max_lat = map_canvas_session.view_max_lat;
                    configurePointLayerGpuDrawState(layer_idx, point_draw_cfg);
                }

                if (!layer.enabled || !layerUsesPolylineGeometry(layer)) {
                    clearPolylineLayerGpuDrawState(layer_idx);
                } else {
                    any_polyline_gpu_layer_enabled = true;
                    ParcelGpuDrawConfig polyline_draw_cfg;
                    polyline_draw_cfg.active = true;
                    polyline_draw_cfg.math_zoom = map_canvas_session.math_zoom;
                    polyline_draw_cfg.zoom_scale = (float)(map_canvas_session.zoom_scale * std::max(1.0f, fb_scale.x));
                    polyline_draw_cfg.center_world = map_canvas_session.center_world;
                    polyline_draw_cfg.viewport_origin =
                        ImVec2(map_canvas_session.origin.x * fb_scale.x, map_canvas_session.origin.y * fb_scale.y);
                    polyline_draw_cfg.viewport_size =
                        ImVec2(map_canvas_session.size.x * fb_scale.x, map_canvas_session.size.y * fb_scale.y);
                    polyline_draw_cfg.framebuffer_size =
                        ImVec2(io.DisplaySize.x * fb_scale.x, io.DisplaySize.y * fb_scale.y);
                    polyline_draw_cfg.view_min_lon = map_canvas_session.view_min_lon;
                    polyline_draw_cfg.view_min_lat = map_canvas_session.view_min_lat;
                    polyline_draw_cfg.view_max_lon = map_canvas_session.view_max_lon;
                    polyline_draw_cfg.view_max_lat = map_canvas_session.view_max_lat;
                    configurePolylineLayerGpuDrawState(layer_idx, polyline_draw_cfg);
                }
            }
            if (!any_zoning_gpu_layer_enabled) {
                clearAllZoningGpuDrawStates();
            }
            if (!any_point_gpu_layer_enabled) {
                clearAllPointLayerGpuDrawStates();
            }
            if (!any_polyline_gpu_layer_enabled) {
                clearAllPolylineLayerGpuDrawStates();
            }
            if (ctx.crime_nibrs_layer_idx >= 0 &&
                (size_t)ctx.crime_nibrs_layer_idx < ctx.layers->size() &&
                (*ctx.layers)[(size_t)ctx.crime_nibrs_layer_idx].enabled) {
                const ImGuiIO& io = ImGui::GetIO();
                const ImVec2 fb_scale = io.DisplayFramebufferScale;
                ParcelGpuDrawConfig crime_draw_cfg;
                crime_draw_cfg.active = true;
                crime_draw_cfg.math_zoom = map_canvas_session.math_zoom;
                crime_draw_cfg.zoom_scale = (float)(map_canvas_session.zoom_scale * std::max(1.0f, fb_scale.x));
                crime_draw_cfg.center_world = map_canvas_session.center_world;
                crime_draw_cfg.viewport_origin =
                    ImVec2(map_canvas_session.origin.x * fb_scale.x, map_canvas_session.origin.y * fb_scale.y);
                crime_draw_cfg.viewport_size =
                    ImVec2(map_canvas_session.size.x * fb_scale.x, map_canvas_session.size.y * fb_scale.y);
                crime_draw_cfg.framebuffer_size =
                    ImVec2(io.DisplaySize.x * fb_scale.x, io.DisplaySize.y * fb_scale.y);
                crime_draw_cfg.view_min_lon = map_canvas_session.view_min_lon;
                crime_draw_cfg.view_min_lat = map_canvas_session.view_min_lat;
                crime_draw_cfg.view_max_lon = map_canvas_session.view_max_lon;
                crime_draw_cfg.view_max_lat = map_canvas_session.view_max_lat;
                configureCrimePointGpuDrawState(crime_draw_cfg);
            } else {
                clearCrimePointGpuDrawState();
            }

            map_canvas_session.map_hovered = map_canvas_session.map_hovered && !map_corner_controls.hovered;
            refreshMapCanvasHoverState(map_canvas_session, map_canvas_ctx);
            if (ctx.hover_debug_state) {
                std::lock_guard<std::mutex> lk(ctx.hover_debug_state->mutex);
                ctx.hover_debug_state->map_hovered = map_canvas_session.map_hovered;
                ctx.hover_debug_state->mouse_screen_x = ImGui::GetIO().MousePos.x;
                ctx.hover_debug_state->mouse_screen_y = ImGui::GetIO().MousePos.y;
                ctx.hover_debug_state->mouse_lon = map_canvas_session.mouse_ll.x;
                ctx.hover_debug_state->mouse_lat = map_canvas_session.mouse_ll.y;
                ctx.hover_debug_state->hovered_parcel = map_canvas_session.hover_state.hovered_parcel_idx != (size_t)-1;
                ctx.hover_debug_state->hovered_parcel_layer_idx = map_canvas_session.hover_state.hovered_parcel_layer_idx;
                ctx.hover_debug_state->hovered_parcel_idx = map_canvas_session.hover_state.hovered_parcel_idx;
                ctx.hover_debug_state->hovered_parcel_entity_id = map_canvas_session.hover_state.hovered_parcel_entity_id;
                ctx.hover_debug_state->hovered_parcel_geometry_entity_id = map_canvas_session.hover_state.hovered_parcel_geometry_entity_id;
                ctx.hover_debug_state->inspect_parcel = map_canvas_session.hover_state.inspect_parcel_idx != (size_t)-1;
                ctx.hover_debug_state->inspect_parcel_layer_idx = map_canvas_session.hover_state.inspect_parcel_layer_idx;
                ctx.hover_debug_state->inspect_parcel_idx = map_canvas_session.hover_state.inspect_parcel_idx;
                ctx.hover_debug_state->inspect_parcel_entity_id = map_canvas_session.hover_state.inspect_parcel_entity_id;
                ctx.hover_debug_state->inspect_parcel_geometry_entity_id = map_canvas_session.hover_state.inspect_parcel_geometry_entity_id;
                ctx.hover_debug_state->hovered_zone = map_canvas_session.hover_state.hovered_zone != nullptr;
                ctx.hover_debug_state->hovered_zone_idx = map_canvas_session.hover_state.hovered_zone_idx;
                ctx.hover_debug_state->hovered_point = map_canvas_session.hover_state.hovered_point != nullptr;
                ctx.hover_debug_state->hovered_point_idx = map_canvas_session.hover_state.hovered_point_idx;
                ctx.hover_debug_state->hovered_point_layer_idx = map_canvas_session.hover_state.hovered_point_layer_idx;
                ctx.hover_debug_state->selected_parcel =
                    ctx.parcel_selection && !ctx.parcel_selection->active_entity_id.empty();
                ctx.hover_debug_state->selected_parcel_layer_idx =
                    ctx.parcel_selection ? ctx.parcel_selection->active_layer_idx : -1;
                ctx.hover_debug_state->selected_parcel_idx =
                    (ctx.parcel_selection && !ctx.parcel_selection->refs.empty() &&
                     ctx.parcel_selection->refs.back().feature_idx != (size_t)-1)
                        ? ctx.parcel_selection->refs.back().feature_idx
                    : (ctx.parcel_selection &&
                       ctx.parcel_selection->active_layer_idx >= 0 &&
                       (size_t)ctx.parcel_selection->active_layer_idx < ctx.layers->size())
                        ? featureIndexForGeometryEntityId(
                            (*ctx.layers)[(size_t)ctx.parcel_selection->active_layer_idx],
                            ctx.parcel_selection->refs.empty()
                                ? ctx.parcel_selection->active_entity_id
                                : ctx.parcel_selection->refs.back().geometry_entity_id)
                        : (size_t)-1;
                ctx.hover_debug_state->selected_parcel_entity_id =
                    ctx.parcel_selection ? ctx.parcel_selection->active_entity_id : std::string();
                ctx.hover_debug_state->selected_parcel_geometry_entity_id =
                    (ctx.parcel_selection && !ctx.parcel_selection->refs.empty())
                        ? ctx.parcel_selection->refs.back().geometry_entity_id
                        : std::string();
                ctx.hover_debug_state->selected_parcel_count =
                    ctx.parcel_selection ? ctx.parcel_selection->entity_ids.size() : 0;
            }

            MapFrameSessionContext map_frame_session_ctx;
            map_frame_session_ctx.root = ctx.root;
            map_frame_session_ctx.duckdb_analytics = ctx.duckdb_analytics;
            map_frame_session_ctx.draw = map_canvas_session.draw;
            map_frame_session_ctx.origin = map_canvas_session.origin;
            map_frame_session_ctx.size = map_canvas_session.size;
            map_frame_session_ctx.center_lon = ctx.center_lon;
            map_frame_session_ctx.center_lat = ctx.center_lat;
            map_frame_session_ctx.zoom_ptr = ctx.zoom;
            map_frame_session_ctx.max_zoom = ctx.max_zoom;
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
            map_frame_session_ctx.heatmap_controls_active = ctx.heatmap_controls_active;
            map_frame_session_ctx.parcel_parameter_mode = ctx.parcel_parameter_mode;
            map_frame_session_ctx.map_polygon_fill_opacity =
                ctx.app_settings ? ctx.app_settings->map_polygon_fill_opacity : 170.0f / 255.0f;
            map_frame_session_ctx.map_hovered = map_canvas_session.map_hovered;
            map_frame_session_ctx.parcel_hover_active = map_canvas_session.parcel_hover_active;
            map_frame_session_ctx.parcel_inspect_active = map_canvas_session.parcel_inspect_active;
            map_frame_session_ctx.zoning_hover_active = map_canvas_session.zoning_hover_active;
            map_frame_session_ctx.zoning_inspect_active = map_canvas_session.zoning_inspect_active;
            map_frame_session_ctx.vacancy_notice_color = &map_canvas_session.vacancy_notice_color;
            map_frame_session_ctx.vacancy_rehab_color = &map_canvas_session.vacancy_rehab_color;
            map_frame_session_ctx.layers = ctx.layers;
            map_frame_session_ctx.point_geometry_artifacts = ctx.point_geometry_artifacts;
            map_frame_session_ctx.polyline_geometry_artifacts = ctx.polyline_geometry_artifacts;
            map_frame_session_ctx.polygon_geometry_artifacts = ctx.polygon_geometry_artifacts;
            map_frame_session_ctx.parcel_render_blob = ctx.parcel_render_blob;
            map_frame_session_ctx.layer_spatial = ctx.layer_spatial;
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
            map_frame_session_ctx.parcel_owner_search_by_feature = ctx.parcel_owner_search_by_feature;
            map_frame_session_ctx.real_property_owner_search_by_feature = ctx.real_property_owner_search_by_feature;
            map_frame_session_ctx.parcel_address_search_by_feature = ctx.parcel_address_search_by_feature;
            map_frame_session_ctx.owner_text_filter_result_set = ctx.owner_text_filter_result_set;
            map_frame_session_ctx.address_text_filter_result_set = ctx.address_text_filter_result_set;
            map_frame_session_ctx.feature_render_cache = ctx.feature_render_cache;
            map_frame_session_ctx.selected_parcel_ids = ctx.selected_parcel_ids;
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
            map_frame_session_ctx.open_parcel_element = [&](const std::string& parcel_entity_id) {
                openElementParcelPage(*ctx.element_info_state, parcel_entity_id);
            };
            map_frame_session_ctx.prof_layer_ms_last = ctx.prof_layer_ms_last;
            map_frame_session_ctx.prof_owner_filter_ms_last = ctx.prof_owner_filter_ms_last;
            map_frame_session_ctx.prof_heatmap_ms_last = ctx.prof_heatmap_ms_last;
            map_frame_session_ctx.prof_owner_filter_candidates_last = ctx.prof_owner_filter_candidates_last;
            map_frame_session_ctx.prof_owner_filter_matches_last = ctx.prof_owner_filter_matches_last;
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
            std::string map_title = ctx.app_settings ? ctx.app_settings->map_title_text : std::string();
            if (ctx.app_settings && ctx.app_settings->map_title_all_caps) {
                map_title = toUpperAsciiCopy(map_title);
            }
            const std::string parcel_source =
                (!trimCopy(map_title).empty() && ctx.app_settings && ctx.app_settings->map_title_show_primary_parcel_source)
                    ? primaryParcelSourceLabel(ctx)
            : std::string();
    drawMapTitleOverlay(map_canvas_session, map_title, parcel_source);

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
    ImGui::End();
}
