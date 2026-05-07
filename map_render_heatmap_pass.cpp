#include "map_render_heatmap_pass.h"

#include "geo.h"

namespace {
void drawHeatCell(ImDrawList* draw, const CachedHeatCell& c, const std::function<ImVec2(const ImVec2&)>& project_world) {
    if (c.is_hex) {
        ImVec2 pts[6] = {
            ImVec2(c.cx - c.hw, c.cy),
            ImVec2(c.cx - c.hw * 0.5f, c.cy - c.hh),
            ImVec2(c.cx + c.hw * 0.5f, c.cy - c.hh),
            ImVec2(c.cx + c.hw, c.cy),
            ImVec2(c.cx + c.hw * 0.5f, c.cy + c.hh),
            ImVec2(c.cx - c.hw * 0.5f, c.cy + c.hh),
        };
        if (c.world_space) {
            for (ImVec2& pt : pts) pt = project_world(pt);
        }
        draw->AddConvexPolyFilled(pts, 6, c.fill);
        if (c.draw_outline) draw->AddPolyline(pts, 6, c.outline, ImDrawFlags_Closed, 1.0f);
    } else {
        ImVec2 p0(c.x0, c.y0);
        ImVec2 p1(c.x1, c.y1);
        if (c.world_space) {
            p0 = project_world(p0);
            p1 = project_world(p1);
        }
        draw->AddRectFilled(p0, p1, c.fill);
        if (c.draw_outline) draw->AddRect(p0, p1, c.outline, 0.0f, 0, 1.0f);
    }
}
}

void drawHeatmapPass(const MapHeatmapDrawContext& ctx) {
    if (!ctx.draw || !ctx.draw_cells || !ctx.project_world) return;
    const bool draw_current_raster =
        ctx.heatmap_raster_texture_valid &&
        ctx.heatmap_raster_cache_key == ctx.heatmap_key;
    const bool drawing_heatmap_raster =
        draw_current_raster &&
        ctx.heatmap_raster_texture &&
        ctx.heatmap_raster_texture->descriptor &&
        ctx.heatmap_raster;
    if (drawing_heatmap_raster) {
        ImVec2 raster_nw = lonLatToWorldPx(ctx.heatmap_raster->min_lon, ctx.heatmap_raster->max_lat, ctx.math_zoom);
        ImVec2 raster_se = lonLatToWorldPx(ctx.heatmap_raster->max_lon, ctx.heatmap_raster->min_lat, ctx.math_zoom);
        ImVec2 rp0 = ctx.project_world(raster_nw);
        ImVec2 rp1 = ctx.project_world(raster_se);
        ctx.draw->AddImage((ImTextureID)ctx.heatmap_raster_texture->descriptor, rp0, rp1);
    }
    const bool suppress_vector_heat_cells = ctx.smooth_only_heatmap && drawing_heatmap_raster;
    if (suppress_vector_heat_cells) return;
    for (const auto& c : *ctx.draw_cells) drawHeatCell(ctx.draw, c, ctx.project_world);
}
