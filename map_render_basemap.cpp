#include "map_render_basemap.h"

#include "geo.h"
#include "worldsim_app_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {
struct BasemapSource {
    std::string label;
    std::string tile_root_dir;
    float opacity = 1.0f;
    bool topo_vector = false;
};

std::string tileUrlTemplate(const std::string& tile_root_dir) {
    if (tile_root_dir == "tiles") return "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
    if (tile_root_dir == "tiles_satellite") {
        return "https://services.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}";
    }
    return "https://services.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/{z}/{y}/{x}";
}

std::string preferredTopoTileDir(const fs::path& root) {
    if (fs::exists(root / "data" / "tiles_topographic")) return "tiles_topographic";
    return "tiles_topo";
}

static int wrapTileX(int x, int period) {
    if (period <= 0) return x;
    x %= period;
    if (x < 0) x += period;
    return x;
}

static int tileXDelta(int a_wrapped, int b_wrapped, int period) {
    if (period <= 0) return a_wrapped - b_wrapped;
    int d = a_wrapped - b_wrapped;
    if (d > period / 2) d -= period;
    if (d < -period / 2) d += period;
    return d;
}

ImU32 imageTint(bool grayscale, float opacity) {
    const int a = std::clamp((int)std::lround(opacity * 255.0f), 0, 255);
    if (grayscale) return IM_COL32(178, 178, 178, a);
    return IM_COL32(255, 255, 255, a);
}

void drawTopoVectorSource(const MapBasemapRenderContext& ctx, float opacity, MapBasemapRenderResult& result) {
    const auto& topo_lines = getTopoVectorLines(*ctx.root);
    const int a = std::clamp((int)std::lround(opacity * 168.0f), 0, 255);
    const ImU32 line_col = ctx.app_settings->grayscale_basemap ? IM_COL32(98, 98, 98, a) : IM_COL32(106, 84, 62, a);
    for (const auto& line_ll : topo_lines) {
        if (line_ll.size() < 2) continue;
        for (size_t i = 1; i < line_ll.size(); ++i) {
            const ImVec2 a_world = lonLatToWorldPx(line_ll[i - 1].x, line_ll[i - 1].y, ctx.math_zoom);
            const ImVec2 b_world = lonLatToWorldPx(line_ll[i].x, line_ll[i].y, ctx.math_zoom);
            const ImVec2 a_screen = ctx.project_world(a_world);
            const ImVec2 b_screen = ctx.project_world(b_world);
            ctx.draw->AddLine(a_screen, b_screen, line_col, 1.0f);
            result.tiles_drawn++;
        }
    }
}

void drawRasterSource(
    const MapBasemapRenderContext& ctx,
    const BasemapSource& source,
    int min_x,
    int max_x,
    int min_y,
    int max_y,
    int period,
    int max_tile,
    MapBasemapRenderResult& result) {
    const std::string tile_url_tmpl = tileUrlTemplate(source.tile_root_dir);
    struct WorkTile {
        int tx;
        int ty;
        int wrapped_x;
        int dx;
        int dy;
        int radius;
    };

    const int center_tx = (int)std::floor(ctx.center_world.x / 256.0);
    const int center_ty = (int)std::floor(ctx.center_world.y / 256.0);
    const int center_wrapped_x = wrapTileX(center_tx, period);
    std::vector<WorkTile> work_tiles;
    work_tiles.reserve(std::max(0, (max_x - min_x + 1) * (max_y - min_y + 1)));

    for (int ty = min_y; ty <= max_y; ++ty) {
        if (ty < 0 || ty > max_tile) continue;
        for (int tx = min_x; tx <= max_x; ++tx) {
            int wrapped_x = tx;
            while (wrapped_x < 0) wrapped_x += period;
            while (wrapped_x > max_tile) wrapped_x -= period;

            const int wrapped_tx = wrapTileX(wrapped_x, period);
            const int dx = tileXDelta(wrapped_tx, center_wrapped_x, period);
            const int dy = ty - center_ty;
            work_tiles.push_back({tx, ty, wrapped_x, dx, dy, std::max(std::abs(dx), std::abs(dy))});
        }
    }

    std::sort(work_tiles.begin(), work_tiles.end(), [](const WorkTile& a, const WorkTile& b) {
        if (a.radius != b.radius) return a.radius < b.radius;
        if (std::abs(a.dx) != std::abs(b.dx)) return std::abs(a.dx) < std::abs(b.dx);
        if (a.dx != b.dx) return a.dx < b.dx;
        if (std::abs(a.dy) != std::abs(b.dy)) return std::abs(a.dy) < std::abs(b.dy);
        return a.ty < b.ty;
    });

    for (const auto& tile : work_tiles) {
        int request_z = std::min(ctx.math_zoom, ctx.max_native_tile_zoom);
        int request_x = tile.wrapped_x;
        int request_y = tile.ty;
        if (ctx.math_zoom > ctx.max_native_tile_zoom) {
            const int dz = ctx.math_zoom - ctx.max_native_tile_zoom;
            const int scale = 1 << std::min(dz, 30);
            request_x = tile.wrapped_x / scale;
            request_y = tile.ty / scale;
        }

        requestLazyBasemapTile(*ctx.lazy_tile_download, *ctx.root, source.tile_root_dir, tile_url_tmpl, request_z, request_x, request_y);
        TileSample sample = getTileSample(*ctx.root, source.tile_root_dir, ctx.math_zoom, tile.wrapped_x, tile.ty);
        if (!sample.tex) continue;

        ImVec2 tile_world((float)(tile.tx * 256), (float)(tile.ty * 256));
        ImVec2 p0 = ctx.project_world(tile_world);
        ImVec2 p1(p0.x + (float)(256.0 * ctx.zoom_scale), p0.y + (float)(256.0 * ctx.zoom_scale));
        ctx.draw->AddImage(
            (ImTextureID)sample.tex->descriptor,
            p0,
            p1,
            sample.uv0,
            sample.uv1,
            imageTint(ctx.app_settings->grayscale_basemap, source.opacity));
        if (ctx.app_settings->grayscale_basemap) {
            const int a = std::clamp((int)std::lround(source.opacity * 78.0f), 0, 255);
            ctx.draw->AddRectFilled(p0, p1, IM_COL32(244, 244, 244, a));
        }
        result.tiles_drawn++;
    }
}
}

MapBasemapRenderResult renderMapBasemap(const MapBasemapRenderContext& ctx) {
    MapBasemapRenderResult result;
    if (!ctx.draw || !ctx.root || !ctx.app_settings || !ctx.lazy_tile_download ||
        !ctx.topo_tiles_available_cached || !ctx.topo_vector_available_cached ||
        !ctx.basemap_source_has_any_files_cached || !ctx.tile_root_dir_cached ||
        !ctx.basemap_availability_last_check || !ctx.project_world) {
        return result;
    }

    const double half_w_world = (ctx.size.x * 0.5) / ctx.zoom_scale;
    const double half_h_world = (ctx.size.y * 0.5) / ctx.zoom_scale;
    const double epsilon = 1e-9;
    int min_x = (int)std::floor((ctx.center_world.x - half_w_world) / 256.0);
    int max_x = (int)std::floor((ctx.center_world.x + half_w_world - epsilon) / 256.0);
    int min_y = (int)std::floor((ctx.center_world.y - half_h_world) / 256.0);
    int max_y = (int)std::floor((ctx.center_world.y + half_h_world - epsilon) / 256.0);
    const int period = 1 << ctx.math_zoom;
    const int max_tile = period - 1;

    const auto now_basemap_check = std::chrono::steady_clock::now();
    if (ctx.basemap_availability_last_check->time_since_epoch().count() == 0 ||
        std::chrono::duration<double>(now_basemap_check - *ctx.basemap_availability_last_check).count() > 5.0) {
        *ctx.basemap_availability_last_check = now_basemap_check;
        *ctx.topo_tiles_available_cached = true;
        *ctx.topo_vector_available_cached = fs::exists(*ctx.root / "data" / "tiles_topo_vector.geojson");
        *ctx.basemap_source_has_any_files_cached = true;
        *ctx.tile_root_dir_cached = ctx.app_settings->basemap_satellite_enabled
            ? "tiles_satellite"
            : (ctx.app_settings->basemap_topographic_enabled ? preferredTopoTileDir(*ctx.root) : "tiles");
    }

    std::vector<BasemapSource> sources;
    if (ctx.app_settings->basemap_satellite_enabled) {
        sources.push_back({"Satellite", "tiles_satellite", std::clamp(ctx.app_settings->basemap_satellite_opacity, 0.0f, 1.0f), false});
    }
    if (ctx.app_settings->basemap_osm_enabled) {
        sources.push_back({"OpenStreetMap", "tiles", std::clamp(ctx.app_settings->basemap_osm_opacity, 0.0f, 1.0f), false});
    }
    if (ctx.app_settings->basemap_topographic_enabled) {
        const bool use_topo_vector = ctx.app_settings->topo_vector_enabled && *ctx.topo_vector_available_cached;
        sources.push_back({"Topographic", preferredTopoTileDir(*ctx.root), std::clamp(ctx.app_settings->basemap_topographic_opacity, 0.0f, 1.0f), use_topo_vector});
    }
    if (sources.empty()) return result;

    for (const auto& source : sources) {
        if (source.opacity <= 0.0f) continue;
        if (source.topo_vector) {
            drawTopoVectorSource(ctx, source.opacity, result);
        } else {
            drawRasterSource(ctx, source, min_x, max_x, min_y, max_y, period, max_tile, result);
        }
    }

    if (ctx.size.x > 0.0f && ctx.size.y > 0.0f &&
        result.tiles_drawn == 0 &&
        ctx.lazy_tile_download->failed.load(std::memory_order_relaxed) > 0) {
        const ImVec2 warn_pos(ctx.origin.x + 14.0f, ctx.origin.y + 14.0f);
        const ImVec2 warn_sz(std::min(560.0f, ctx.size.x - 28.0f), 94.0f);
        ctx.draw->AddRectFilled(warn_pos, ImVec2(warn_pos.x + warn_sz.x, warn_pos.y + warn_sz.y), IM_COL32(120, 18, 14, 220), 6.0f);
        ctx.draw->AddRect(warn_pos, ImVec2(warn_pos.x + warn_sz.x, warn_pos.y + warn_sz.y), IM_COL32(255, 180, 170, 230), 6.0f, 0, 1.0f);
        ctx.draw->AddText(ImVec2(warn_pos.x + 10.0f, warn_pos.y + 10.0f), IM_COL32(255, 235, 232, 255), "Basemap unavailable");
        ctx.draw->AddText(ImVec2(warn_pos.x + 10.0f, warn_pos.y + 32.0f), IM_COL32(255, 220, 216, 255),
                          "No enabled basemap tiles are available yet.");
        ctx.draw->AddText(ImVec2(warn_pos.x + 10.0f, warn_pos.y + 52.0f), IM_COL32(255, 220, 216, 255),
                          "Visible tiles are downloaded lazily and cached on disk.");
        ctx.draw->AddText(ImVec2(warn_pos.x + 10.0f, warn_pos.y + 72.0f), IM_COL32(255, 220, 216, 255),
                          "Layer overlays may still render if local layer files are present.");
    }

    return result;
}
