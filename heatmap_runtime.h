#pragma once

#include "aggregate_visualization_strategies.h"
#include "heat_normalization.h"
#include "heatmap_render.h"
#include "map_render_heatmap_pass.h"
#include "worldsim_app_internal.h"

#include <atomic>
#include <filesystem>
#include <future>
#include <functional>
#include <unordered_map>
#include <vector>

struct CachedAggregateTexture {
    std::vector<CachedHeatCell> cells;
    HeatmapRaster raster;
    TileTexture texture;
    uint64_t last_used_frame = 0;
};

struct CachedHeatNormalization {
    HeatNormalizationState state;
    uint64_t last_used_frame = 0;
};

struct HeatmapRuntimeState {
    float global_heat_cell_px = 24.0f;
    int heatmap_algo = kAggregateNone;
    int heatmap_quality_preset = 1;
    float heatmap_bandwidth_px = 18.0f;
    float heatmap_blur_sigma_px = 6.0f;
    float heatmap_percentile_clip = 95.0f;
    bool heatmap_zoom_adaptive_bandwidth = true;
    bool heatmap_multires_enabled = true;
    float heatmap_multires_blend = 0.5f;
    bool heatmap_allow_cpu_fallback = false;

    std::vector<CachedHeatCell> cached_cells;
    TileTexture raster_texture;
    bool raster_texture_valid = false;
    HeatmapRaster cached_raster_meta;
    uint64_t raster_cache_key = 0;
    uint64_t cache_key = 0;
    bool cache_valid = false;
    std::future<std::pair<uint64_t, HeatmapRenderData>> async_future;
    bool async_inflight = false;
    uint64_t pending_key = 0;
    std::unordered_map<uint64_t, CachedAggregateTexture> texture_cache;
    uint64_t texture_cache_frame = 0;
    std::unordered_map<uint64_t, CachedHeatNormalization> normalization_cache;
};

struct HeatmapCacheLookup {
    const CachedAggregateTexture* cached_aggregate_for_key = nullptr;
    bool can_use_cached_heatmap = false;
    bool aggregate_generation_pending = false;
};

struct HeatmapFramePassContext {
    const std::filesystem::path* root = nullptr;
    HeatmapRuntimeState* runtime = nullptr;
    const std::vector<HeatSample>* heat_samples = nullptr;
    ImDrawList* draw = nullptr;
    ImVec2 origin = ImVec2(0.0f, 0.0f);
    ImVec2 size = ImVec2(0.0f, 0.0f);
    float view_min_lon = 0.0f;
    float view_min_lat = 0.0f;
    float view_max_lon = 0.0f;
    float view_max_lat = 0.0f;
    int zoom = 0;
    int math_zoom = 0;
    int heatmap_algo = kAggregateNone;
    float global_heat_cell = 24.0f;
    float heatmap_bandwidth_px = 18.0f;
    float heatmap_blur_sigma_px = 6.0f;
    float heatmap_percentile_clip = 95.0f;
    bool heatmap_zoom_adaptive_bandwidth = true;
    bool heatmap_multires_enabled = true;
    float heatmap_multires_blend = 0.5f;
    bool should_recompute_heatmap = false;
    bool any_active_heatmap = false;
    bool any_active_gpu_splat = false;
    bool smooth_only_heatmap = false;
    bool heatmap_async_inflight = false;
    bool can_use_cached_heatmap = false;
    bool high_quality_gpu_aggregate = false;
    uint64_t heatmap_key = 0;
    int smooth_heat_raster_base_px = 0;
    int smooth_heat_raster_max_px = 0;
    const CachedAggregateTexture* cached_aggregate_for_key = nullptr;
    std::function<ImVec2(const ImVec2&)> project_world;
    std::atomic<bool>* prof_heatmap_gpu_splat_active = nullptr;
    std::atomic<bool>* prof_heatmap_high_quality = nullptr;
    std::atomic<bool>* prof_heatmap_cache_valid = nullptr;
    std::atomic<bool>* prof_heatmap_texture_resident = nullptr;
    std::atomic<bool>* prof_heatmap_async_inflight = nullptr;
    std::atomic<uint64_t>* prof_heatmap_cache_key = nullptr;
    std::atomic<size_t>* prof_heatmap_texture_cache_entries = nullptr;
};

void clearHeatmapRuntimeCache(HeatmapRuntimeState& state);
void destroyHeatmapRuntimeTexturesNow(HeatmapRuntimeState& state);
HeatmapCacheLookup prepareHeatmapAggregateCache(
    const std::filesystem::path& root,
    HeatmapRuntimeState& state,
    bool any_active_heatmap,
    bool smooth_only_heatmap,
    uint64_t heatmap_key);
void runHeatmapFramePass(const HeatmapFramePassContext& ctx);
