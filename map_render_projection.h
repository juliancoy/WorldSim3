#pragma once

#include "imgui.h"
#include "types.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

struct MapFillStats {
    size_t attempts = 0;
    size_t success = 0;
    size_t no_triangles = 0;
    size_t bad_indices = 0;
};

class MapProjectionCache {
public:
    MapProjectionCache(int math_zoom, int ring_step, std::function<ImVec2(const ImVec2&)> project_world);

    const std::vector<std::vector<ImVec2>>& getWorldRings(
        size_t layer_idx,
        uint32_t feature_idx,
        const LayerDef::FeatureGeom& fg);

    const std::pair<ImVec2, ImVec2>& getWorldExtent(
        size_t layer_idx,
        uint32_t feature_idx,
        const LayerDef::FeatureGeom& fg);

    void reserveWorldRings(size_t count);

    void appendWorldRingLine(const std::vector<ImVec2>& world_ring);
    void appendWorldRingLine(const std::vector<ImVec2>& world_ring, int ring_step);
    ImVec2 projectWorld(const ImVec2& world) const { return project_world_(world); }

    bool drawTessellatedFill(
        ImDrawList* draw,
        const LayerDef::FeatureGeom& fg,
        const std::vector<std::vector<ImVec2>>& world_rings,
        ImU32 fill_color);

    const std::vector<ImVec2>& scratchLine() const { return scratch_line_; }
    const MapFillStats& fillStats() const { return fill_stats_; }

private:
    uint64_t featureCacheKey(size_t layer_idx, uint32_t feature_idx) const;
    size_t projectWorldRingsForFill(const std::vector<std::vector<ImVec2>>& world_rings);

    int math_zoom_ = 0;
    int ring_step_ = 1;
    std::function<ImVec2(const ImVec2&)> project_world_;
    std::unordered_map<uint64_t, std::vector<std::vector<ImVec2>>> world_rings_cache_;
    std::unordered_map<uint64_t, std::pair<ImVec2, ImVec2>> world_extent_cache_;
    std::pair<ImVec2, ImVec2> last_world_extent_;
    std::vector<ImVec2> scratch_fill_verts_;
    std::vector<uint32_t> scratch_fill_indices_;
    std::vector<ImVec2> scratch_line_;
    MapFillStats fill_stats_;
};
