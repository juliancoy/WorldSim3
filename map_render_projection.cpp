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
    scratch_fill_indices_.reserve(12288);
    scratch_line_.reserve(1024);
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
    const LayerDef::FeatureGeom& fg,
    const std::vector<std::vector<ImVec2>>& world_rings,
    ImU32 fill_color) {
    fill_stats_.attempts++;
    if (fg.triangles.empty()) {
        fill_stats_.no_triangles++;
        return false;
    }
    // Very dense polygons dominate CPU at low zoom; outline-only is acceptable there.
    if (math_zoom_ <= 12 && fg.triangles.size() > 6000) return false;
    const size_t vcount = projectWorldRingsForFill(world_rings);
    if (vcount < 3) return false;

    scratch_fill_indices_.clear();
    scratch_fill_indices_.reserve(fg.triangles.size());
    for (size_t ti = 0; ti + 2 < fg.triangles.size(); ti += 3) {
        const uint32_t a = fg.triangles[ti + 0];
        const uint32_t b = fg.triangles[ti + 1];
        const uint32_t cidx = fg.triangles[ti + 2];
        if (a < vcount && b < vcount && cidx < vcount) {
            scratch_fill_indices_.push_back(a);
            scratch_fill_indices_.push_back(b);
            scratch_fill_indices_.push_back(cidx);
        }
    }
    if (scratch_fill_indices_.empty()) {
        fill_stats_.bad_indices++;
        return false;
    }

    const ImDrawListFlags old_flags = draw->Flags;
    draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (size_t ii = 0; ii + 2 < scratch_fill_indices_.size(); ii += 3) {
        draw->AddTriangleFilled(
            scratch_fill_verts_[scratch_fill_indices_[ii + 0]],
            scratch_fill_verts_[scratch_fill_indices_[ii + 1]],
            scratch_fill_verts_[scratch_fill_indices_[ii + 2]],
            fill_color);
    }
    draw->Flags = old_flags;
    fill_stats_.success++;
    return true;
}

uint64_t MapProjectionCache::featureCacheKey(size_t layer_idx, uint32_t feature_idx) const {
    return (uint64_t(layer_idx) << 32) | uint64_t(feature_idx);
}

size_t MapProjectionCache::projectWorldRingsForFill(const std::vector<std::vector<ImVec2>>& world_rings) {
    size_t total = 0;
    for (const auto& r : world_rings) total += r.size();
    scratch_fill_verts_.clear();
    scratch_fill_verts_.reserve(total);
    for (const auto& r : world_rings) {
        for (const ImVec2& wp : r) scratch_fill_verts_.push_back(project_world_(wp));
    }
    return total;
}
