#pragma once

#include "cache_io.h"
#include "layer_runtime.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct PolygonArtifactRuntimeState {
    std::unordered_map<size_t, PolygonGeometryArtifact> geometry_artifacts;
    std::unordered_map<size_t, std::string> artifact_signatures;
};

struct PolygonArtifactRuntimeSyncInput {
    const std::filesystem::path* root = nullptr;
    const std::vector<LayerDef>* layers = nullptr;
    std::vector<LayerRuntimeState>* layer_states = nullptr;
    int parcel_layer_idx = -1;
    const std::unordered_map<size_t, std::string>* gpu_uploaded_signatures = nullptr;
};

void syncPolygonGeometryArtifacts(
    const PolygonArtifactRuntimeSyncInput& input,
    PolygonArtifactRuntimeState& state);
