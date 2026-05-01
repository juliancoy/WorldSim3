#pragma once

#include "types.h"

#include <filesystem>
#include <unordered_map>
#include <vector>

std::vector<LayerDef> loadManifest(const std::filesystem::path& root);

void loadLayerUiState(
    const std::filesystem::path& root,
    std::vector<LayerDef>& layers,
    bool& parcel_hover_enabled,
    std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr);

void saveLayerUiState(
    const std::filesystem::path& root,
    const std::vector<LayerDef>& layers,
    bool parcel_hover_enabled,
    const std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr);
