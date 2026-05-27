#include "map_render_heatmap_pass.h"

#include "aggregate_debug.h"
#include "geo.h"

#include <algorithm>
#include <cstdio>
#include <cmath>

namespace {
ImVec2 normalizedMin(const ImVec2& a, const ImVec2& b) {
    return ImVec2(std::min(a.x, b.x), std::min(a.y, b.y));
}

ImVec2 normalizedMax(const ImVec2& a, const ImVec2& b) {
    return ImVec2(std::max(a.x, b.x), std::max(a.y, b.y));
}

bool isDrawableRect(const ImVec2& min_p, const ImVec2& max_p) {
    return std::isfinite(min_p.x) && std::isfinite(min_p.y) &&
        std::isfinite(max_p.x) && std::isfinite(max_p.y) &&
        max_p.x > min_p.x && max_p.y > min_p.y;
}

void logHeatmapDrawSkip(uint64_t key, const char* reason, size_t raster_layers, int texture_descriptor) {
    if (!worldsimGpuAggregateDebugEnabled()) return;
    static uint64_t last_key = 0;
    static const char* last_reason = nullptr;
    if (last_key == key && last_reason == reason) return;
    last_key = key;
    last_reason = reason;
    std::fprintf(
        stderr,
        "[worldsim3][gpu-aggregate] draw-skip key=%llu reason=%s raster_layers=%zu texture_desc=%d\n",
        (unsigned long long)key,
        reason,
        raster_layers,
        texture_descriptor);
}

void logHeatmapDrawImage(uint64_t key, const ImVec2& min_p, const ImVec2& max_p) {
    if (!worldsimGpuAggregateDebugEnabled()) return;
    static uint64_t last_key = 0;
    static int last_x = 0;
    static int last_y = 0;
    static int last_w = 0;
    static int last_h = 0;
    const int x = (int)std::round(min_p.x);
    const int y = (int)std::round(min_p.y);
    const int w = (int)std::round(max_p.x - min_p.x);
    const int h = (int)std::round(max_p.y - min_p.y);
    if (last_key == key && last_x == x && last_y == y && last_w == w && last_h == h) return;
    last_key = key;
    last_x = x;
    last_y = y;
    last_w = w;
    last_h = h;
    std::fprintf(
        stderr,
        "[worldsim3][gpu-aggregate] draw-image key=%llu screen=(%.1f,%.1f)-(%.1f,%.1f) size=%dx%d\n",
        (unsigned long long)key,
        min_p.x,
        min_p.y,
        max_p.x,
        max_p.y,
        w,
        h);
}

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
    bool drew_raster = false;
    if (draw_current_raster && ctx.heatmap_raster_layers && !ctx.heatmap_raster_layers->empty()) {
        for (const auto& layer : *ctx.heatmap_raster_layers) {
            if (!layer.has_gpu_texture || !layer.gpu_texture.descriptor) continue;
            ImVec2 raster_nw = lonLatToWorldPx(layer.raster.min_lon, layer.raster.max_lat, ctx.math_zoom);
            ImVec2 raster_se = lonLatToWorldPx(layer.raster.max_lon, layer.raster.min_lat, ctx.math_zoom);
            ImVec2 rp0 = ctx.project_world(raster_nw);
            ImVec2 rp1 = ctx.project_world(raster_se);
            ImVec2 min_p = normalizedMin(rp0, rp1);
            ImVec2 max_p = normalizedMax(rp0, rp1);
            if (!isDrawableRect(min_p, max_p)) {
                logHeatmapDrawSkip(
                    ctx.heatmap_key,
                    "invalid-layer-rect",
                    ctx.heatmap_raster_layers->size(),
                    1);
                continue;
            }
            ctx.draw->AddImage((ImTextureID)layer.gpu_texture.descriptor, min_p, max_p);
            logHeatmapDrawImage(ctx.heatmap_key, min_p, max_p);
            drew_raster = true;
        }
        if (!drew_raster) {
            logHeatmapDrawSkip(
                ctx.heatmap_key,
                "no-layer-descriptor",
                ctx.heatmap_raster_layers->size(),
                0);
        }
    }
    const bool drawing_heatmap_raster =
        draw_current_raster &&
        !drew_raster &&
        ctx.heatmap_raster_texture &&
        ctx.heatmap_raster_texture->descriptor &&
        ctx.heatmap_raster;
    if (drawing_heatmap_raster) {
        ImVec2 raster_nw = lonLatToWorldPx(ctx.heatmap_raster->min_lon, ctx.heatmap_raster->max_lat, ctx.math_zoom);
        ImVec2 raster_se = lonLatToWorldPx(ctx.heatmap_raster->max_lon, ctx.heatmap_raster->min_lat, ctx.math_zoom);
        ImVec2 rp0 = ctx.project_world(raster_nw);
        ImVec2 rp1 = ctx.project_world(raster_se);
        ImVec2 min_p = normalizedMin(rp0, rp1);
        ImVec2 max_p = normalizedMax(rp0, rp1);
        if (isDrawableRect(min_p, max_p)) {
            ctx.draw->AddImage((ImTextureID)ctx.heatmap_raster_texture->descriptor, min_p, max_p);
            logHeatmapDrawImage(ctx.heatmap_key, min_p, max_p);
        } else {
            logHeatmapDrawSkip(
                ctx.heatmap_key,
                "invalid-texture-rect",
                ctx.heatmap_raster_layers ? ctx.heatmap_raster_layers->size() : 0,
                1);
        }
    } else if (draw_current_raster && !drew_raster) {
        logHeatmapDrawSkip(
            ctx.heatmap_key,
            "no-raster-descriptor",
            ctx.heatmap_raster_layers ? ctx.heatmap_raster_layers->size() : 0,
            ctx.heatmap_raster_texture && ctx.heatmap_raster_texture->descriptor ? 1 : 0);
    }
    const bool suppress_vector_heat_cells = ctx.smooth_only_heatmap && (drawing_heatmap_raster || drew_raster);
    if (suppress_vector_heat_cells) return;
    for (const auto& c : *ctx.draw_cells) drawHeatCell(ctx.draw, c, ctx.project_world);
}
