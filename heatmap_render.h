#pragma once

#include "imgui.h"

#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

struct CachedHeatCell {
    bool world_space = false;
    bool is_hex = false;
    bool draw_outline = true;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    float cx = 0.0f, cy = 0.0f, hw = 0.0f, hh = 0.0f;
    ImU32 fill = 0;
    ImU32 outline = 0;
};

struct HeatmapRaster {
    int w = 0;
    int h = 0;
    float min_lon = 0.0f;
    float min_lat = 0.0f;
    float max_lon = 0.0f;
    float max_lat = 0.0f;
    std::vector<unsigned char> rgba;
};

struct HeatmapRenderData {
    std::vector<CachedHeatCell> cells;
    HeatmapRaster raster;
    bool has_raster = false;
};

struct HeatSample {
    int layer = -1;
    float x = 0.0f;
    float y = 0.0f;
    float lon = 0.0f;
    float lat = 0.0f;
    ImVec4 color = ImVec4(1, 1, 1, 1);
    float value = 0.0f;
    bool has_value = false;
    bool prefer_gradient = true;
    int algo = 0;
    float cell_px = 24.0f;
    float bandwidth_px = 18.0f;
    float blur_sigma_px = 6.0f;
    float percentile_clip = 95.0f;
    bool zoom_adaptive_bandwidth = true;
    bool multires_enabled = true;
    float multires_blend = 0.5f;
    bool allow_cpu_fallback = false;
};

std::pair<uint64_t, HeatmapRenderData> buildHeatmapRenderData(
    uint64_t key,
    std::vector<HeatSample> samples,
    float origin_x,
    float origin_y,
    float view_min_lon,
    float view_min_lat,
    float view_max_lon,
    float view_max_lat,
    float viewport_w,
    float viewport_h,
    int zoom,
    int max_zoom,
    int raster_base_px,
    int raster_max_px);

bool loadHeatmapRasterCache(
    const std::filesystem::path& cache_path,
    uint64_t key,
    HeatmapRaster& out);

void saveHeatmapRasterCache(
    const std::filesystem::path& cache_path,
    uint64_t key,
    const HeatmapRaster& raster);

std::vector<CachedHeatCell> buildImmediateHeatmapCells(
    const std::vector<HeatSample>& heat_samples,
    float origin_x,
    float origin_y,
    float viewport_w,
    float viewport_h,
    int zoom,
    int max_zoom,
    int heatmap_algo,
    float global_heat_cell,
    float heatmap_bandwidth_px,
    float heatmap_blur_sigma_px,
    float heatmap_percentile_clip,
    bool heatmap_zoom_adaptive_bandwidth,
    bool heatmap_multires_enabled,
    float heatmap_multires_blend);
