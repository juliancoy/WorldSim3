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

struct PointLayerRuntimeState {
    std::unordered_map<size_t, uint64_t> color_state_keys;
    std::unordered_map<size_t, uint64_t> glyph_state_keys;
    std::unordered_map<size_t, PointGeometryArtifact> geometry_artifacts;
    std::unordered_map<size_t, std::string> artifact_signatures;
};

struct PointLayerRuntimeSyncInput {
    const std::filesystem::path* root = nullptr;
    const std::vector<LayerDef>* layers = nullptr;
    std::vector<LayerRuntimeState>* layer_states = nullptr;
    const MapFilterState* map_filter_state = nullptr;
    const std::vector<QueryMapLayer>* query_layers = nullptr;
    uint64_t feature_render_state_key = 0;
    int crime_nibrs_layer_idx = -1;
    std::function<const LayerFeatureRenderCache&()> ensure_feature_render_cache;
};

void syncPointGpuLayers(const PointLayerRuntimeSyncInput& input, PointLayerRuntimeState& state);
