#pragma once

#include "app_settings.h"
#include "cache_io.h"
#include "filters.h"
#include "layer_runtime.h"

#include "imgui.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct ZoningRuntimeState {
    std::unordered_map<size_t, std::string> uploaded_signatures;
    std::unordered_map<size_t, std::string> failed_signatures;
    std::unordered_map<size_t, uint64_t> color_state_keys;
    std::unordered_map<size_t, uint64_t> outline_state_keys;
    std::unordered_map<size_t, std::vector<ImU32>> last_base_colors;
    std::unordered_map<size_t, std::vector<ImU32>> last_outline_colors;
    std::unordered_map<size_t, ParcelRenderCacheBlob> render_blobs;
    std::unordered_map<size_t, std::vector<LayerDef::FeatureRecord>> color_features;
    std::unordered_map<size_t, std::vector<LayerDef::FeatureProperties>> color_feature_properties;
};

struct ZoningRuntimeSyncInput {
    const std::filesystem::path* root = nullptr;
    const AppSettings* app_settings = nullptr;
    const std::vector<LayerDef>* layers = nullptr;
    std::vector<LayerRuntimeState>* layer_states = nullptr;
    int parcel_layer_idx = -1;
    const MapFilterState* map_filter_state = nullptr;
    const std::vector<QueryMapLayer>* query_layers = nullptr;
    const std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr;
    const std::unordered_map<std::string, ImVec4>* zoning_zone_color = nullptr;
    const std::vector<bool>* layer_fill_enabled = nullptr;
    const std::vector<bool>* layer_heatmap_use_gradient = nullptr;
    const std::vector<float>* layer_choropleth_gamma = nullptr;
    const std::vector<int>* layer_normalize_mode = nullptr;
    const std::vector<float>* layer_heatmap_percentile_clip = nullptr;
    float heatmap_percentile_clip = 95.0f;
    uint64_t feature_render_state_key = 0;
    std::function<const LayerFeatureRenderCache&()> ensure_feature_render_cache;
};

void syncZoningGpuLayers(const ZoningRuntimeSyncInput& input, ZoningRuntimeState& state);
