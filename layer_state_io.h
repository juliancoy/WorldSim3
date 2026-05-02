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
    std::vector<bool>* layer_inspect_enabled = nullptr);

void saveLayerUiState(
    const std::filesystem::path& root,
    const std::vector<LayerDef>& layers,
    bool hover_inspector_enabled,
    const std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr,
    const std::vector<bool>* layer_fill_enabled = nullptr,
    const std::vector<bool>* layer_hover_enabled = nullptr,
    const std::vector<bool>* layer_inspect_enabled = nullptr);
