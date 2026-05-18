#include "heatmap_runtime.h"

#include "heatmap_render.h"
#include "map_render_hud.h"
#include "worldsim_app.h"

#include <algorithm>
#include <cmath>
#include <future>
#include <sstream>

namespace {

constexpr size_t kMaxHeatmapTextureCacheEntries = 8;
std::filesystem::path aggregateCachePath(const std::filesystem::path& root, uint64_t key) {
    std::ostringstream name;
    name << std::hex << key << ".raster.bin";
    return root / "data" / "cache" / "aggregate" / name.str();
}

void pruneHeatmapTextureCache(HeatmapRuntimeState& state) {
    while (state.texture_cache.size() > kMaxHeatmapTextureCacheEntries) {
        auto evict_it = state.texture_cache.begin();
        for (auto it = state.texture_cache.begin(); it != state.texture_cache.end(); ++it) {
            if (it->second.last_used_frame < evict_it->second.last_used_frame) evict_it = it;
        }
        destroyTileTexture(evict_it->second.texture);
        state.texture_cache.erase(evict_it);
    }
}

} // namespace

void clearHeatmapRuntimeCache(HeatmapRuntimeState& state) {
    for (auto& kv : state.texture_cache) destroyTileTexture(kv.second.texture);
    state.texture_cache.clear();
    state.cached_cells.clear();
    state.cached_raster_meta = {};
    state.cache_key = 0;
    state.cache_valid = false;
    state.raster_cache_key = 0;
    if (state.raster_texture.descriptor) destroyTileTexture(state.raster_texture);
    state.raster_texture_valid = false;
    state.texture_cache_frame = 0;
    state.normalization_cache.clear();
    if (state.async_inflight) {
        state.async_inflight = false;
        state.pending_key = 0;
        state.async_future = {};
    }
}

void destroyHeatmapRuntimeTexturesNow(HeatmapRuntimeState& state) {
    for (auto& kv : state.texture_cache) destroyTileTextureNow(kv.second.texture);
    state.texture_cache.clear();
    if (state.raster_texture.descriptor) destroyTileTextureNow(state.raster_texture);
    state.raster_texture = {};
    state.raster_texture_valid = false;
}

HeatmapCacheLookup prepareHeatmapAggregateCache(
    const std::filesystem::path& root,
    HeatmapRuntimeState& state,
    bool any_active_heatmap,
    bool smooth_only_heatmap,
    uint64_t heatmap_key) {
    if (state.async_inflight && state.async_future.valid() &&
        state.async_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        auto done = state.async_future.get();
        state.async_inflight = false;
        state.pending_key = 0;
        CachedAggregateTexture aggregate_entry;
        aggregate_entry.cells = std::move(done.second.cells);
        aggregate_entry.last_used_frame = ++state.texture_cache_frame;
        bool aggregate_has_texture = false;
        if (done.second.has_raster && done.second.raster.w > 0 && done.second.raster.h > 0 && !done.second.raster.rgba.empty()) {
            saveHeatmapRasterCache(aggregateCachePath(root, done.first), done.first, done.second.raster);
            aggregate_entry.raster = done.second.raster;
            aggregate_has_texture = uploadRgbaTexture(
                done.second.raster.rgba.data(),
                (uint32_t)done.second.raster.w,
                (uint32_t)done.second.raster.h,
                aggregate_entry.texture);
            if (aggregate_has_texture) recordGpuProfilerEvent("heatmap raster upload");
        }
        const bool aggregate_has_content = aggregate_has_texture || !aggregate_entry.cells.empty();
        if (aggregate_has_content || !any_active_heatmap) {
            auto old_cached_aggregate = state.texture_cache.find(done.first);
            if (old_cached_aggregate != state.texture_cache.end()) {
                destroyTileTexture(old_cached_aggregate->second.texture);
                state.texture_cache.erase(old_cached_aggregate);
            }
            state.texture_cache.emplace(done.first, std::move(aggregate_entry));
            pruneHeatmapTextureCache(state);
            if (done.first == heatmap_key) {
                const auto current_cached_aggregate = state.texture_cache.find(done.first);
                if (current_cached_aggregate != state.texture_cache.end()) {
                    state.cached_cells = current_cached_aggregate->second.cells;
                    state.cached_raster_meta = current_cached_aggregate->second.raster;
                    state.raster_cache_key = aggregate_has_texture ? done.first : 0;
                    state.raster_texture_valid = aggregate_has_texture;
                }
                if (!aggregate_has_texture && !smooth_only_heatmap) {
                    destroyTileTexture(state.raster_texture);
                    state.raster_texture_valid = false;
                    state.cached_raster_meta = {};
                    state.raster_cache_key = 0;
                }
                state.cache_key = done.first;
                state.cache_valid = true;
            }
        }
    }

    if (!any_active_heatmap) {
        state.cached_cells.clear();
        state.cache_valid = false;
        state.cache_key = 0;
        state.pending_key = 0;
        if (state.raster_texture_valid) destroyTileTexture(state.raster_texture);
        state.raster_texture_valid = false;
        state.cached_raster_meta = {};
        state.raster_cache_key = 0;
        return {};
    }

    HeatmapCacheLookup lookup;
    auto cached_it = state.texture_cache.find(heatmap_key);
    if (cached_it != state.texture_cache.end()) {
        cached_it->second.last_used_frame = ++state.texture_cache_frame;
        lookup.cached_aggregate_for_key = &cached_it->second;
        state.cache_key = heatmap_key;
        state.cache_valid = true;
    } else if (!state.async_inflight) {
        HeatmapRaster disk_raster;
        if (loadHeatmapRasterCache(aggregateCachePath(root, heatmap_key), heatmap_key, disk_raster)) {
            CachedAggregateTexture disk_entry;
            disk_entry.raster = std::move(disk_raster);
            disk_entry.last_used_frame = ++state.texture_cache_frame;
            if (uploadRgbaTexture(
                    disk_entry.raster.rgba.data(),
                    (uint32_t)disk_entry.raster.w,
                    (uint32_t)disk_entry.raster.h,
                    disk_entry.texture)) {
                recordGpuProfilerEvent("heatmap raster cache upload");
                state.texture_cache.emplace(heatmap_key, std::move(disk_entry));
                pruneHeatmapTextureCache(state);
                auto current = state.texture_cache.find(heatmap_key);
                if (current != state.texture_cache.end()) {
                    lookup.cached_aggregate_for_key = &current->second;
                    state.cache_key = heatmap_key;
                    state.cache_valid = true;
                }
            }
        }
    }

    lookup.can_use_cached_heatmap =
        lookup.cached_aggregate_for_key != nullptr ||
        (state.cache_valid && state.cache_key == heatmap_key);
    lookup.aggregate_generation_pending = state.async_inflight && state.pending_key != 0;
    return lookup;
}

void runHeatmapFramePass(const HeatmapFramePassContext& ctx) {
    HeatmapRuntimeState& state = *ctx.runtime;
    bool should_recompute_heatmap = ctx.should_recompute_heatmap;
    if (should_recompute_heatmap && ctx.heat_samples->empty()) {
        state.cached_cells.clear();
        state.cache_key = ctx.heatmap_key;
        state.cache_valid = true;
        should_recompute_heatmap = false;
    }

    if (should_recompute_heatmap && !state.async_inflight) {
        const auto samples_copy = *ctx.heat_samples;
        const uint64_t key_copy = ctx.heatmap_key;
        const int zoom_copy = ctx.zoom;
        const float vw = std::max(1.0f, ctx.size.x);
        const float vh = std::max(1.0f, ctx.size.y);
        const float view_min_lon = ctx.view_min_lon;
        const float view_min_lat = ctx.view_min_lat;
        const float view_max_lon = ctx.view_max_lon;
        const float view_max_lat = ctx.view_max_lat;
        const int smooth_heat_raster_base_px = ctx.smooth_heat_raster_base_px;
        const int smooth_heat_raster_max_px = ctx.smooth_heat_raster_max_px;
        state.async_future = std::async(std::launch::async, [=]() mutable -> std::pair<uint64_t, HeatmapRenderData> {
            return buildHeatmapRenderData(
                key_copy,
                samples_copy,
                0.0f,
                0.0f,
                view_min_lon,
                view_min_lat,
                view_max_lon,
                view_max_lat,
                vw,
                vh,
                zoom_copy,
                kMaxZoom,
                smooth_heat_raster_base_px,
                smooth_heat_raster_max_px);
        });
        state.async_inflight = true;
        state.pending_key = key_copy;
        if (ctx.smooth_only_heatmap) should_recompute_heatmap = false;
    } else if (state.async_inflight) {
        should_recompute_heatmap = false;
    }

    std::vector<CachedHeatCell> frame_heat_cells;
    if (should_recompute_heatmap) {
        frame_heat_cells = buildImmediateHeatmapCells(
            *ctx.heat_samples,
            0.0f,
            0.0f,
            ctx.size.x,
            ctx.size.y,
            ctx.zoom,
            kMaxZoom,
            ctx.heatmap_algo,
            ctx.global_heat_cell,
            ctx.heatmap_bandwidth_px,
            ctx.heatmap_blur_sigma_px,
            ctx.heatmap_percentile_clip,
            ctx.heatmap_zoom_adaptive_bandwidth,
            ctx.heatmap_multires_enabled,
            ctx.heatmap_multires_blend);
        state.cached_cells = frame_heat_cells;
        state.cache_key = ctx.heatmap_key;
        state.cache_valid = true;
    }

    const CachedAggregateTexture* draw_cached_aggregate = ctx.cached_aggregate_for_key;
    uint64_t draw_cached_aggregate_key = ctx.heatmap_key;
    if (!draw_cached_aggregate && state.async_inflight && state.cache_valid && state.cache_key != 0) {
        auto stale_it = state.texture_cache.find(state.cache_key);
        if (stale_it != state.texture_cache.end() && stale_it->second.texture.descriptor) {
            stale_it->second.last_used_frame = ++state.texture_cache_frame;
            draw_cached_aggregate = &stale_it->second;
            draw_cached_aggregate_key = state.cache_key;
        }
    }

    const std::vector<CachedHeatCell> empty_heat_cells;
    const std::vector<CachedHeatCell>& draw_cells =
        draw_cached_aggregate ? draw_cached_aggregate->cells :
        (ctx.can_use_cached_heatmap ? state.cached_cells :
        (should_recompute_heatmap ? frame_heat_cells : empty_heat_cells));
    const bool cached_aggregate_has_texture =
        draw_cached_aggregate && draw_cached_aggregate->texture.descriptor;

    MapHeatmapDrawContext heatmap_draw_ctx;
    heatmap_draw_ctx.draw = ctx.draw;
    heatmap_draw_ctx.math_zoom = ctx.math_zoom;
    heatmap_draw_ctx.smooth_only_heatmap = ctx.any_active_heatmap && ctx.smooth_only_heatmap;
    heatmap_draw_ctx.heatmap_async_inflight = ctx.any_active_heatmap && state.async_inflight;
    heatmap_draw_ctx.heatmap_raster_texture_valid =
        ctx.any_active_heatmap && (cached_aggregate_has_texture || state.raster_texture_valid);
    heatmap_draw_ctx.heatmap_raster_cache_key =
        cached_aggregate_has_texture ? draw_cached_aggregate_key : state.raster_cache_key;
    heatmap_draw_ctx.heatmap_key =
        cached_aggregate_has_texture ? draw_cached_aggregate_key : ctx.heatmap_key;
    heatmap_draw_ctx.heatmap_raster_texture =
        cached_aggregate_has_texture ? &draw_cached_aggregate->texture : &state.raster_texture;
    heatmap_draw_ctx.heatmap_raster =
        draw_cached_aggregate ? &draw_cached_aggregate->raster : &state.cached_raster_meta;
    heatmap_draw_ctx.draw_cells = &draw_cells;
    heatmap_draw_ctx.project_world = ctx.project_world;

    if (ctx.prof_heatmap_gpu_splat_active) {
        ctx.prof_heatmap_gpu_splat_active->store(ctx.any_active_gpu_splat, std::memory_order_relaxed);
    }
    if (ctx.prof_heatmap_high_quality) {
        ctx.prof_heatmap_high_quality->store(ctx.high_quality_gpu_aggregate, std::memory_order_relaxed);
    }
    if (ctx.prof_heatmap_cache_valid) {
        ctx.prof_heatmap_cache_valid->store(ctx.can_use_cached_heatmap, std::memory_order_relaxed);
    }
    if (ctx.prof_heatmap_texture_resident) {
        ctx.prof_heatmap_texture_resident->store(
            heatmap_draw_ctx.heatmap_raster_texture_valid &&
            heatmap_draw_ctx.heatmap_raster_texture &&
            heatmap_draw_ctx.heatmap_raster_texture->descriptor,
            std::memory_order_relaxed);
    }
    if (ctx.prof_heatmap_async_inflight) {
        ctx.prof_heatmap_async_inflight->store(state.async_inflight, std::memory_order_relaxed);
    }
    if (ctx.prof_heatmap_cache_key) {
        ctx.prof_heatmap_cache_key->store(state.cache_valid ? state.cache_key : 0, std::memory_order_relaxed);
    }
    if (ctx.prof_heatmap_texture_cache_entries) {
        ctx.prof_heatmap_texture_cache_entries->store(state.texture_cache.size(), std::memory_order_relaxed);
    }

    drawHeatmapPass(heatmap_draw_ctx);
    if (ctx.any_active_gpu_splat && state.async_inflight && !ctx.can_use_cached_heatmap) {
        drawMapStatusBadge(ctx.draw, ctx.origin, "Generating Aggregate");
    }
}
