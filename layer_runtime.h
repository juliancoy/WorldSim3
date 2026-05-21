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

struct SpatialIndexJob {
    size_t index = 0;
    std::string source_signature;
    std::vector<LayerDef::FeatureExtent> feature_extents;
};

enum class LayerPipelineStatus {
    Queued,
    Hydrating,
    Hydrated,
    Ready,
    Failed
};

struct LayerRuntimeState {
    LayerPipelineStatus status = LayerPipelineStatus::Queued;
    size_t feature_count = 0;
    std::string error;
    GeometryArtifactClass geometry_artifact_class = GeometryArtifactClass::Unknown;
    std::string geometry_artifact_path;
    std::string geometry_source_signature;
    std::string geometry_phase;
    std::string hydration_source_signature;
    std::string spatial_index_source_signature;
    std::string hydration_source_kind;
    std::string hydration_phase;
    std::string spatial_index_phase;
    bool geometry_loaded_from_artifact = false;
    bool geometry_gpu_resident = false;
    bool geometry_gpu_pick_ready = false;
    bool hydration_loaded_from_cache = false;
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
std::string layerRuntimeDisplayStatus(const LayerRuntimeState& state, const std::string& layer_file = {});
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
