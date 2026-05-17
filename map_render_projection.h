#pragma once

#include "imgui.h"
#include "types.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct MapFillStats {
    size_t attempts = 0;
    size_t success = 0;
    size_t no_triangles = 0;
    size_t bad_indices = 0;
};

struct CachedWorldFillGeometry {
    std::vector<ImVec2> vertices;
    std::vector<uint32_t> triangle_indices;
};

struct CachedFeatureColorStorage {
    uint64_t style_key = 0;
    ImU32 feature_color = 0;
    std::vector<ImU32> polygon_colors;
};

class MapProjectionCache {
public:
    MapProjectionCache(int math_zoom, int ring_step, std::function<ImVec2(const ImVec2&)> project_world);
    void updateFrameProjection(int math_zoom, int ring_step, std::function<ImVec2(const ImVec2&)> project_world);
    void clearCachedGeometry();

    const std::vector<std::vector<ImVec2>>& getWorldRings(
        size_t layer_idx,
        uint32_t feature_idx,
        const LayerDef::FeatureGeom& fg);

    const std::pair<ImVec2, ImVec2>& getWorldExtent(
        size_t layer_idx,
        uint32_t feature_idx,
        const LayerDef::FeatureGeom& fg);

    void reserveWorldRings(size_t count);
    void setLowZoomDenseFillLayers(const std::vector<size_t>& layer_indices);

    void appendWorldRingLine(const std::vector<ImVec2>& world_ring);
    void appendWorldRingLine(const std::vector<ImVec2>& world_ring, int ring_step);
    ImVec2 projectWorld(const ImVec2& world) const { return project_world_(world); }

    const CachedWorldFillGeometry& getWorldFillGeometry(
        size_t layer_idx,
        uint32_t feature_idx,
        const LayerDef::FeatureGeom& fg);

    const CachedFeatureColorStorage* findFeatureColorStorage(
        size_t layer_idx,
        uint32_t feature_idx,
        uint64_t style_key) const;
    void storeFeatureColorStorage(
        size_t layer_idx,
        uint32_t feature_idx,
        uint64_t style_key,
        ImU32 feature_color,
        size_t polygon_count);

    bool drawTessellatedFill(
        ImDrawList* draw,
        size_t layer_idx,
        uint32_t feature_idx,
        const LayerDef::FeatureGeom& fg,
        ImU32 fill_color);

    const std::vector<ImVec2>& scratchLine() const { return scratch_line_; }
    const MapFillStats& fillStats() const { return fill_stats_; }
    size_t cachedWorldRingEntries() const { return world_rings_cache_.size(); }
    size_t cachedWorldExtentEntries() const { return world_extent_cache_.size(); }
    size_t cachedWorldFillEntries() const { return world_fill_cache_.size(); }
    size_t cachedFeatureColorEntries() const { return feature_color_cache_.size(); }

private:
    uint64_t featureCacheKey(size_t layer_idx, uint32_t feature_idx) const;
    size_t projectWorldVerticesForFill(const std::vector<ImVec2>& world_vertices);

    int math_zoom_ = 0;
    int ring_step_ = 1;
    std::function<ImVec2(const ImVec2&)> project_world_;
    std::unordered_map<uint64_t, std::vector<std::vector<ImVec2>>> world_rings_cache_;
    std::unordered_map<uint64_t, std::pair<ImVec2, ImVec2>> world_extent_cache_;
    std::unordered_map<uint64_t, CachedWorldFillGeometry> world_fill_cache_;
    std::unordered_map<uint64_t, CachedFeatureColorStorage> feature_color_cache_;
    std::unordered_set<size_t> low_zoom_dense_fill_layers_;
    std::pair<ImVec2, ImVec2> last_world_extent_;
    std::vector<ImVec2> scratch_fill_verts_;
    std::vector<ImVec2> scratch_line_;
    MapFillStats fill_stats_;
};
