#include "map_canvas_session.h"

#include "app_utils.h"
#include "layer_geometry.h"
#include "map_render_basemap.h"

#include <chrono>

namespace {
double profMsSince(const std::chrono::steady_clock::time_point& begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

bool layerToggleEnabled(const std::vector<bool>* values, int idx) {
    return values && idx >= 0 && (size_t)idx < values->size() && (*values)[(size_t)idx];
}

bool activeLayerMatches(int active_layer_idx, int layer_idx) {
    return active_layer_idx >= 0 && active_layer_idx == layer_idx;
}

bool isParcelInteractionLayer(const std::vector<LayerDef>& layers, int layer_idx) {
    if (layer_idx < 0 || (size_t)layer_idx >= layers.size()) return false;
    const LayerDef& layer = layers[(size_t)layer_idx];
    return layer.enabled &&
           layer.scale == "parcel" &&
           !layerUsesPointGeometry(layer) &&
           !layerUsesPolylineGeometry(layer);
}

bool anyParcelInteractionLayerEnabled(const std::vector<LayerDef>& layers, const std::vector<bool>* values) {
    if (!values) return false;
    const size_t count = std::min(layers.size(), values->size());
    for (size_t i = 0; i < count; ++i) {
        if (!(*values)[i]) continue;
        if (isParcelInteractionLayer(layers, (int)i)) return true;
    }
    return false;
}
}

MapCanvasSession beginMapCanvasSession(const MapCanvasSessionContext& ctx) {
    MapCanvasSession session;
    if (!ctx.center_lon || !ctx.center_lat || !ctx.zoom || !ctx.root || !ctx.app_settings ||
        !ctx.lazy_tile_download || !ctx.topo_tiles_available_cached || !ctx.topo_vector_available_cached ||
        !ctx.basemap_source_has_any_files_cached || !ctx.tile_root_dir_cached || !ctx.basemap_availability_last_check ||
        !ctx.layers || !ctx.layer_spatial || !ctx.layer_hover_enabled || !ctx.layer_inspect_enabled ||
        !ctx.prof_tiles_drawn_frame || !ctx.prof_tile_ms_last ||
        !ctx.persistent_projection_cache || !ctx.persistent_projection_generation ||
        !ctx.prof_projection_world_ring_cache_entries || !ctx.prof_projection_world_extent_cache_entries ||
        !ctx.prof_projection_cache_generation) {
        return session;
    }

    const MapViewportFrame map_viewport = beginMapViewportCanvas(MapViewportContext{
        ctx.center_lon,
        ctx.center_lat,
        ctx.zoom,
        ctx.app_settings->zoom_step,
        ctx.min_zoom,
        ctx.max_zoom,
        ctx.max_internal_math_zoom,
        ctx.app_settings->dark_mode
    });
    session.draw = map_viewport.draw;
    session.origin = map_viewport.origin;
    session.size = map_viewport.size;
    session.map_hovered = map_viewport.hovered;
    session.map_active = map_viewport.active;
    session.math_zoom = map_viewport.math_zoom;
    session.zoom_scale = map_viewport.zoom_scale;
    session.center_world = map_viewport.center_world;
    session.view_min_lon = map_viewport.view_min_lon;
    session.view_max_lon = map_viewport.view_max_lon;
    session.view_min_lat = map_viewport.view_min_lat;
    session.view_max_lat = map_viewport.view_max_lat;
    session.project_world = [map_viewport](const ImVec2& wp) { return map_viewport.projectWorld(wp); };

    session.parcel_hover_active =
        anyParcelInteractionLayerEnabled(*ctx.layers, ctx.layer_hover_enabled);
    session.parcel_inspect_active =
        anyParcelInteractionLayerEnabled(*ctx.layers, ctx.layer_inspect_enabled);
    session.zoning_hover_active =
        activeLayerMatches(ctx.active_hover_layer_idx, ctx.zoning_layer_idx) &&
        layerToggleEnabled(ctx.layer_hover_enabled, ctx.zoning_layer_idx);
    session.zoning_inspect_active =
        activeLayerMatches(ctx.active_click_layer_idx, ctx.zoning_layer_idx) &&
        layerToggleEnabled(ctx.layer_inspect_enabled, ctx.zoning_layer_idx);
    const bool hover_points_enabled =
        ctx.active_hover_layer_idx >= 0 &&
        ctx.active_hover_layer_idx != ctx.parcel_layer_idx &&
        ctx.active_hover_layer_idx != ctx.zoning_layer_idx &&
        layerToggleEnabled(ctx.layer_hover_enabled, ctx.active_hover_layer_idx);
    const bool inspect_points_enabled =
        ctx.active_click_layer_idx >= 0 &&
        ctx.active_click_layer_idx != ctx.parcel_layer_idx &&
        ctx.active_click_layer_idx != ctx.zoning_layer_idx &&
        layerToggleEnabled(ctx.layer_inspect_enabled, ctx.active_click_layer_idx);

    const bool suppress_hover_lookup =
        session.map_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f);
    MapHoverQuery hover_query;
    hover_query.map_hovered = session.map_hovered && !suppress_hover_lookup;
    hover_query.parcel_hover_active = session.parcel_hover_active;
    hover_query.parcel_inspect_active = session.parcel_inspect_active;
    hover_query.zoning_hover_active = session.zoning_hover_active;
    hover_query.zoning_inspect_active = session.zoning_inspect_active;
    hover_query.point_hover_active = hover_points_enabled;
    hover_query.point_inspect_active = inspect_points_enabled;
    hover_query.active_hover_layer_idx = ctx.active_hover_layer_idx;
    hover_query.active_click_layer_idx = ctx.active_click_layer_idx;
    hover_query.parcel_layer_idx = ctx.parcel_layer_idx;
    hover_query.zoning_layer_idx = ctx.zoning_layer_idx;
    hover_query.layers = ctx.layers;
    hover_query.point_geometry_artifacts = ctx.point_geometry_artifacts;
    hover_query.polygon_geometry_artifacts = ctx.polygon_geometry_artifacts;
    hover_query.parcel_render_blob = ctx.parcel_render_blob;
    hover_query.layer_spatial = ctx.layer_spatial;
    hover_query.layer_hover_enabled = ctx.layer_hover_enabled;
    hover_query.layer_inspect_enabled = ctx.layer_inspect_enabled;
    hover_query.mouse_ll = map_viewport.mouse_ll;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 fb_scale = io.DisplayFramebufferScale;
    hover_query.mouse_screen = ImVec2(io.MousePos.x * fb_scale.x, io.MousePos.y * fb_scale.y);
    hover_query.center_world = session.center_world;
    hover_query.viewport_origin = ImVec2(session.origin.x * fb_scale.x, session.origin.y * fb_scale.y);
    hover_query.viewport_size = ImVec2(session.size.x * fb_scale.x, session.size.y * fb_scale.y);
    hover_query.framebuffer_size = ImVec2(io.DisplaySize.x * fb_scale.x, io.DisplaySize.y * fb_scale.y);
    hover_query.view_min_lon = session.view_min_lon;
    hover_query.view_max_lon = session.view_max_lon;
    hover_query.view_min_lat = session.view_min_lat;
    hover_query.view_max_lat = session.view_max_lat;
    hover_query.math_zoom = session.math_zoom;
    hover_query.zoom_scale = (float)(session.zoom_scale * std::max(1.0f, fb_scale.x));
    hover_query.project_world = session.project_world;
    session.hover_state = findMapHoverTargets(hover_query);
    session.mouse_ll = map_viewport.mouse_ll;

    const auto tile_prof_begin = std::chrono::steady_clock::now();
    const MapBasemapRenderResult basemap_result = renderMapBasemap(MapBasemapRenderContext{
        session.draw,
        session.origin,
        session.size,
        map_viewport.center_world,
        session.math_zoom,
        session.zoom_scale,
        ctx.root,
        ctx.app_settings,
        ctx.lazy_tile_download,
        ctx.topo_tiles_available_cached,
        ctx.topo_vector_available_cached,
        ctx.basemap_source_has_any_files_cached,
        ctx.tile_root_dir_cached,
        ctx.basemap_availability_last_check,
        ctx.max_native_tile_zoom,
        session.project_world
    });
    *ctx.prof_tiles_drawn_frame += basemap_result.tiles_drawn;
    ctx.prof_tile_ms_last->store(profMsSince(tile_prof_begin), std::memory_order_relaxed);

    session.vacant_notice_enabled =
        ctx.vacant_notice_layer_idx >= 0 &&
        (size_t)ctx.vacant_notice_layer_idx < ctx.layers->size() &&
        (*ctx.layers)[(size_t)ctx.vacant_notice_layer_idx].enabled;
    session.vacant_rehab_enabled =
        ctx.vacant_rehab_layer_idx >= 0 &&
        (size_t)ctx.vacant_rehab_layer_idx < ctx.layers->size() &&
        (*ctx.layers)[(size_t)ctx.vacant_rehab_layer_idx].enabled;
    session.tax_lien_enabled =
        ctx.tax_lien_layer_idx >= 0 &&
        (size_t)ctx.tax_lien_layer_idx < ctx.layers->size() &&
        (*ctx.layers)[(size_t)ctx.tax_lien_layer_idx].enabled;
    session.tax_sale_enabled =
        ctx.tax_sale_layer_idx >= 0 &&
        (size_t)ctx.tax_sale_layer_idx < ctx.layers->size() &&
        (*ctx.layers)[(size_t)ctx.tax_sale_layer_idx].enabled;
    session.vacancy_notice_color =
        (ctx.vacant_notice_layer_idx >= 0 && (size_t)ctx.vacant_notice_layer_idx < ctx.layers->size())
            ? (*ctx.layers)[(size_t)ctx.vacant_notice_layer_idx].color
            : ImVec4(1, 0, 0, 1);
    session.vacancy_rehab_color =
        (ctx.vacant_rehab_layer_idx >= 0 && (size_t)ctx.vacant_rehab_layer_idx < ctx.layers->size())
            ? (*ctx.layers)[(size_t)ctx.vacant_rehab_layer_idx].color
            : ImVec4(0, 1, 1, 1);

    session.zoom = ctx.zoom ? *ctx.zoom : (double)session.math_zoom;
    session.lod_ring_step = lodRingStepForZoom((int)std::floor(session.zoom));
    session.should_fill_layer_polygon = [layers = ctx.layers](size_t layer_idx) {
        if (!layers || layer_idx >= layers->size()) return false;
        return true;
    };
    if (*ctx.persistent_projection_generation != ctx.projection_generation) {
        ctx.persistent_projection_cache->reset();
        *ctx.persistent_projection_generation = ctx.projection_generation;
    }
    if (!*ctx.persistent_projection_cache) {
        *ctx.persistent_projection_cache = std::make_unique<MapProjectionCache>(session.math_zoom, 1, session.project_world);
        (*ctx.persistent_projection_cache)->reserveWorldRings(8192);
    } else {
        (*ctx.persistent_projection_cache)->updateFrameProjection(session.math_zoom, 1, session.project_world);
    }
    std::vector<size_t> low_zoom_dense_fill_layers;
    for (size_t i = 0; i < ctx.layers->size(); ++i) {
        if ((*ctx.layers)[i].category == LayerDef::Category::Zoning) low_zoom_dense_fill_layers.push_back(i);
    }
    (*ctx.persistent_projection_cache)->setLowZoomDenseFillLayers(low_zoom_dense_fill_layers);
    ctx.prof_projection_cache_generation->store(*ctx.persistent_projection_generation, std::memory_order_relaxed);
    session.projection_cache = ctx.persistent_projection_cache->get();
    return session;
}
