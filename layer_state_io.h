#pragma once

#include "types.h"

#include <filesystem>
#include <unordered_map>
#include <vector>

std::vector<LayerDef> loadManifest(const std::filesystem::path& root);

void loadLayerUiState(
    const std::filesystem::path& root,
    std::vector<LayerDef>& layers,
    bool& hover_inspector_enabled,
    std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr,
    std::vector<bool>* layer_fill_enabled = nullptr,
    std::vector<bool>* layer_hover_enabled = nullptr,
    std::vector<bool>* layer_inspect_enabled = nullptr,
    std::vector<bool>* layer_heatmap_enabled = nullptr,
    std::vector<int>* layer_heatmap_max_zoom = nullptr,
    std::vector<int>* layer_parcel_detail_min_zoom = nullptr,
    std::vector<bool>* layer_heatmap_use_gradient = nullptr,
    std::vector<int>* layer_heatmap_algo = nullptr,
    std::vector<int>* layer_normalize_mode = nullptr,
    std::vector<float>* layer_heatmap_cell_px = nullptr,
    std::vector<float>* layer_heatmap_bandwidth_px = nullptr,
    std::vector<float>* layer_heatmap_blur_sigma_px = nullptr,
    std::vector<float>* layer_heatmap_percentile_clip = nullptr,
    std::vector<bool>* layer_heatmap_zoom_adaptive_bandwidth = nullptr,
    std::vector<bool>* layer_heatmap_multires_enabled = nullptr,
    std::vector<float>* layer_heatmap_multires_blend = nullptr,
    int* heatmap_algo = nullptr,
    int* heatmap_quality_preset = nullptr,
    float* heatmap_cell_px = nullptr,
    float* heatmap_bandwidth_px = nullptr,
    float* heatmap_blur_sigma_px = nullptr,
    float* heatmap_percentile_clip = nullptr,
    bool* heatmap_zoom_adaptive_bandwidth = nullptr,
    bool* heatmap_multires_enabled = nullptr,
    float* heatmap_multires_blend = nullptr);

void saveLayerUiState(
    const std::filesystem::path& root,
    const std::vector<LayerDef>& layers,
    bool hover_inspector_enabled,
    const std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr,
    const std::vector<bool>* layer_fill_enabled = nullptr,
    const std::vector<bool>* layer_hover_enabled = nullptr,
    const std::vector<bool>* layer_inspect_enabled = nullptr,
    const std::vector<bool>* layer_heatmap_enabled = nullptr,
    const std::vector<int>* layer_heatmap_max_zoom = nullptr,
    const std::vector<int>* layer_parcel_detail_min_zoom = nullptr,
    const std::vector<bool>* layer_heatmap_use_gradient = nullptr,
    const std::vector<int>* layer_heatmap_algo = nullptr,
    const std::vector<int>* layer_normalize_mode = nullptr,
    const std::vector<float>* layer_heatmap_cell_px = nullptr,
    const std::vector<float>* layer_heatmap_bandwidth_px = nullptr,
    const std::vector<float>* layer_heatmap_blur_sigma_px = nullptr,
    const std::vector<float>* layer_heatmap_percentile_clip = nullptr,
    const std::vector<bool>* layer_heatmap_zoom_adaptive_bandwidth = nullptr,
    const std::vector<bool>* layer_heatmap_multires_enabled = nullptr,
    const std::vector<float>* layer_heatmap_multires_blend = nullptr,
    const int* heatmap_algo = nullptr,
    const int* heatmap_quality_preset = nullptr,
    const float* heatmap_cell_px = nullptr,
    const float* heatmap_bandwidth_px = nullptr,
    const float* heatmap_blur_sigma_px = nullptr,
    const float* heatmap_percentile_clip = nullptr,
    const bool* heatmap_zoom_adaptive_bandwidth = nullptr,
    const bool* heatmap_multires_enabled = nullptr,
    const float* heatmap_multires_blend = nullptr);
