#pragma once

#include "cache_io.h"
#include "filters.h"
#include "layer_runtime.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct CrimePointRuntimeState {
    std::string uploaded_signature;
    uint64_t color_state_key = 0;
    PointGeometryArtifact artifact;
};

struct CrimePointRuntimeSyncInput {
    const std::filesystem::path* root = nullptr;
    const std::vector<LayerDef>* layers = nullptr;
    std::vector<LayerRuntimeState>* layer_states = nullptr;
    int crime_nibrs_layer_idx = -1;
    const MapFilterState* map_filter_state = nullptr;
    const std::vector<QueryMapLayer>* query_layers = nullptr;
    uint64_t feature_render_state_key = 0;
    std::function<const LayerFeatureRenderCache&()> ensure_feature_render_cache;
};

void syncCrimePointGpuLayer(const CrimePointRuntimeSyncInput& input, CrimePointRuntimeState& state);

void publishCrimePointArtifactForAggregateSampling(
    int crime_nibrs_layer_idx,
    const CrimePointRuntimeState& crime_state,
    std::unordered_map<size_t, PointGeometryArtifact>& point_geometry_artifacts,
    std::unordered_map<size_t, std::string>* point_artifact_signatures = nullptr);
