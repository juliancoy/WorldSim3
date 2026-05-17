#include "map_render_projection.h"

#include "geo.h"

#include <algorithm>

MapProjectionCache::MapProjectionCache(
    int math_zoom,
    int ring_step,
    std::function<ImVec2(const ImVec2&)> project_world)
    : math_zoom_(math_zoom),
      ring_step_(std::max(1, ring_step)),
      project_world_(std::move(project_world)) {
    scratch_fill_verts_.reserve(4096);
    scratch_line_.reserve(1024);
}

void MapProjectionCache::updateFrameProjection(
    int math_zoom,
    int ring_step,
    std::function<ImVec2(const ImVec2&)> project_world) {
    const bool zoom_changed = math_zoom_ != math_zoom;
    math_zoom_ = math_zoom;
    ring_step_ = std::max(1, ring_step);
    project_world_ = std::move(project_world);
    if (zoom_changed) clearCachedGeometry();
}

void MapProjectionCache::clearCachedGeometry() {
    world_rings_cache_.clear();
    world_extent_cache_.clear();
    world_fill_cache_.clear();
}

const std::vector<std::vector<ImVec2>>& MapProjectionCache::getWorldRings(
    size_t layer_idx,
    uint32_t feature_idx,
    const LayerDef::FeatureGeom& fg) {
    const uint64_t key = featureCacheKey(layer_idx, feature_idx);
    auto it = world_rings_cache_.find(key);
    if (it != world_rings_cache_.end()) return it->second;
    std::vector<std::vector<ImVec2>> rings;
    rings.reserve(fg.rings.size());
    for (const auto& r : fg.rings) {
        std::vector<ImVec2> rr;
        rr.reserve(r.size());
        for (const ImVec2& ll : r) rr.push_back(lonLatToWorldPx(ll.x, ll.y, math_zoom_));
        rings.push_back(std::move(rr));
    }
    return world_rings_cache_.emplace(key, std::move(rings)).first->second;
}

const std::pair<ImVec2, ImVec2>& MapProjectionCache::getWorldExtent(
    size_t layer_idx,
    uint32_t feature_idx,
    const LayerDef::FeatureGeom& fg) {
    const uint64_t key = featureCacheKey(layer_idx, feature_idx);
    auto it = world_extent_cache_.find(key);
    if (it != world_extent_cache_.end()) return it->second;
    last_world_extent_ = {
        lonLatToWorldPx(fg.extent.min_lon, fg.extent.max_lat, math_zoom_),
        lonLatToWorldPx(fg.extent.max_lon, fg.extent.min_lat, math_zoom_)
    };
    return world_extent_cache_.emplace(key, last_world_extent_).first->second;
}

void MapProjectionCache::reserveWorldRings(size_t count) {
    world_rings_cache_.reserve(count);
}

void MapProjectionCache::setLowZoomDenseFillLayers(const std::vector<size_t>& layer_indices) {
    low_zoom_dense_fill_layers_.clear();
    low_zoom_dense_fill_layers_.insert(layer_indices.begin(), layer_indices.end());
}

const CachedWorldFillGeometry& MapProjectionCache::getWorldFillGeometry(
    size_t layer_idx,
    uint32_t feature_idx,
    const LayerDef::FeatureGeom& fg) {
    const uint64_t key = featureCacheKey(layer_idx, feature_idx);
    auto it = world_fill_cache_.find(key);
    if (it != world_fill_cache_.end()) return it->second;

    const auto& world_rings = getWorldRings(layer_idx, feature_idx, fg);
    CachedWorldFillGeometry cached;
    size_t total = 0;
    for (const auto& ring : world_rings) total += ring.size();
    cached.vertices.reserve(total);
    for (const auto& ring : world_rings) {
        cached.vertices.insert(cached.vertices.end(), ring.begin(), ring.end());
    }
    cached.triangle_indices.reserve(fg.triangles.size());
    const size_t vcount = cached.vertices.size();
    for (size_t ti = 0; ti + 2 < fg.triangles.size(); ti += 3) {
        const uint32_t a = fg.triangles[ti + 0];
        const uint32_t b = fg.triangles[ti + 1];
        const uint32_t cidx = fg.triangles[ti + 2];
        if (a < vcount && b < vcount && cidx < vcount) {
            cached.triangle_indices.push_back(a);
            cached.triangle_indices.push_back(b);
            cached.triangle_indices.push_back(cidx);
        }
    }
    return world_fill_cache_.emplace(key, std::move(cached)).first->second;
}

const CachedFeatureColorStorage* MapProjectionCache::findFeatureColorStorage(
    size_t layer_idx,
    uint32_t feature_idx,
    uint64_t style_key) const {
    const uint64_t key = featureCacheKey(layer_idx, feature_idx);
    auto it = feature_color_cache_.find(key);
    if (it == feature_color_cache_.end()) return nullptr;
    if (it->second.style_key != style_key) return nullptr;
    return &it->second;
}

void MapProjectionCache::storeFeatureColorStorage(
    size_t layer_idx,
    uint32_t feature_idx,
    uint64_t style_key,
    ImU32 feature_color,
    size_t polygon_count) {
    const uint64_t key = featureCacheKey(layer_idx, feature_idx);
    auto& cached = feature_color_cache_[key];
    cached.style_key = style_key;
    cached.feature_color = feature_color;
    cached.polygon_colors.assign(std::max<size_t>(polygon_count, 0), feature_color);
}

void MapProjectionCache::appendWorldRingLine(const std::vector<ImVec2>& world_ring) {
    appendWorldRingLine(world_ring, ring_step_);
}

void MapProjectionCache::appendWorldRingLine(const std::vector<ImVec2>& world_ring, int ring_step) {
    scratch_line_.clear();
    if (world_ring.empty()) return;
    const int step = std::max(1, ring_step);
    const size_t n = world_ring.size();
    if (step == 1 || n <= 4) {
        for (const ImVec2& wp : world_ring) scratch_line_.push_back(project_world_(wp));
    } else {
        scratch_line_.reserve((n / (size_t)step) + 2);
        for (size_t i = 0; i < n; i += (size_t)step) scratch_line_.push_back(project_world_(world_ring[i]));
        if ((n - 1) % (size_t)step != 0) scratch_line_.push_back(project_world_(world_ring.back()));
    }
}

bool MapProjectionCache::drawTessellatedFill(
    ImDrawList* draw,
    size_t layer_idx,
    uint32_t feature_idx,
    const LayerDef::FeatureGeom& fg,
    ImU32 fill_color) {
    fill_stats_.attempts++;
    if (fg.triangles.empty()) {
        fill_stats_.no_triangles++;
        return false;
    }
    // Very dense parcel polygons dominate CPU at low zoom, but zoning is a semantic
    // area layer and must keep filled class color at every zoom.
    if (math_zoom_ <= 12 && fg.triangles.size() > 6000 && !low_zoom_dense_fill_layers_.count(layer_idx)) return false;
    const auto& cached = getWorldFillGeometry(layer_idx, feature_idx, fg);
    const size_t vcount = projectWorldVerticesForFill(cached.vertices);
    if (vcount < 3) return false;

    if (cached.triangle_indices.empty()) {
        fill_stats_.bad_indices++;
        return false;
    }

    const ImDrawListFlags old_flags = draw->Flags;
    draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (size_t ii = 0; ii + 2 < cached.triangle_indices.size(); ii += 3) {
        draw->AddTriangleFilled(
            scratch_fill_verts_[cached.triangle_indices[ii + 0]],
            scratch_fill_verts_[cached.triangle_indices[ii + 1]],
            scratch_fill_verts_[cached.triangle_indices[ii + 2]],
            fill_color);
    }
    draw->Flags = old_flags;
    fill_stats_.success++;
    return true;
}

uint64_t MapProjectionCache::featureCacheKey(size_t layer_idx, uint32_t feature_idx) const {
    return (uint64_t(layer_idx) << 32) | uint64_t(feature_idx);
}

size_t MapProjectionCache::projectWorldVerticesForFill(const std::vector<ImVec2>& world_vertices) {
    const size_t total = world_vertices.size();
    scratch_fill_verts_.clear();
    scratch_fill_verts_.reserve(total);
    for (const ImVec2& wp : world_vertices) scratch_fill_verts_.push_back(project_world_(wp));
    return total;
}
