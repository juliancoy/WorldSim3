#pragma once

#include "heatmap_render.h"
#include "imgui.h"
#include "worldsim_app_internal.h"

#include <cstdint>
#include <functional>
#include <vector>

struct MapHeatmapDrawContext {
    ImDrawList* draw = nullptr;
    int math_zoom = 0;
    bool smooth_only_heatmap = false;
    bool heatmap_async_inflight = false;
    bool heatmap_raster_texture_valid = false;
    uint64_t heatmap_raster_cache_key = 0;
    uint64_t heatmap_key = 0;
    const TileTexture* heatmap_raster_texture = nullptr;
    const HeatmapRaster* heatmap_raster = nullptr;
    const std::vector<CachedHeatCell>* draw_cells = nullptr;
    std::function<ImVec2(const ImVec2&)> project_world;
};

void drawHeatmapPass(const MapHeatmapDrawContext& ctx);
