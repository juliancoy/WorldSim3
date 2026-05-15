#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct HydratedLayer {
    size_t index = 0;
    std::vector<LayerDef::FeatureGeom> features;
    bool done = false;
    bool failed = false;
    bool replace_existing = false;
    bool loaded_from_cache = false;
    std::string error;
    std::string source_signature;
};

struct TriJob {
    size_t index = 0;
    std::string file;
    std::string source_signature;
    std::vector<std::vector<std::vector<ImVec2>>> rings_per_feature;
};

struct TriResult {
    size_t index = 0;
    std::string source_signature;
    std::vector<std::vector<uint32_t>> triangles_per_feature;
    size_t apply_offset = 0;
    bool ok = true;
    bool loaded_from_cache = false;
    bool loaded_from_binary_cache = false;
    std::string error;
};

struct SpatialIndexJob {
    size_t index = 0;
    std::string source_signature;
    std::vector<LayerDef::FeatureExtent> feature_extents;
};

enum class LayerPipelineStatus {
    Queued,
    Hydrating,
    Hydrated,
    TriQueued,
    Triangulating,
    Ready,
    Failed
};

struct LayerRuntimeState {
    LayerPipelineStatus status = LayerPipelineStatus::Queued;
    size_t feature_count = 0;
    std::string error;
    std::string hydration_source_signature;
    std::string triangulation_source_signature;
    std::string spatial_index_source_signature;
    std::string hydration_phase;
    std::string triangulation_phase;
    std::string spatial_index_phase;
    bool hydration_loaded_from_cache = false;
    bool triangulation_loaded_from_cache = false;
};

struct LayerSpatialIndex {
    bool built = false;
    size_t feature_count_built = 0;
    float min_lon = 0.0f;
    float min_lat = 0.0f;
    float max_lon = 0.0f;
    float max_lat = 0.0f;
    int nx = 0;
    int ny = 0;
    std::vector<std::vector<uint32_t>> cells;
    std::vector<uint32_t> marks;
    uint32_t mark_id = 1;
};

struct SpatialIndexResult {
    size_t index = 0;
    std::string source_signature;
    size_t feature_count = 0;
    LayerSpatialIndex spatial_index;
    bool ok = true;
    std::string error;
};

const char* statusToString(LayerPipelineStatus s);
void buildLayerSpatialIndex(const LayerDef& layer, LayerSpatialIndex& si);
void buildLayerSpatialIndexForExtents(
    const std::vector<LayerDef::FeatureExtent>& feature_extents,
    LayerSpatialIndex& si);
bool queryLayerSpatialIndex(
    LayerSpatialIndex& si,
    float q_min_lon,
    float q_min_lat,
    float q_max_lon,
    float q_max_lat,
    std::vector<uint32_t>& out);
