#pragma once

#include "app_settings.h"
#include "imgui.h"
#include "zoning.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct ZoningFiltersPanelContext {
    int zoning_layer_idx = -1;
    const std::filesystem::path* root = nullptr;
    AppSettings* app_settings = nullptr;
    std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr;
    std::unordered_map<std::string, ImVec4>* zoning_zone_color = nullptr;
    std::unordered_map<std::string, std::string>* zoning_zone_label = nullptr;
    const std::unordered_map<std::string, ZoneMetadata>* zoning_metadata = nullptr;
    const std::vector<std::string>* zoning_zone_order = nullptr;
    const std::unordered_map<std::string, size_t>* zoning_zone_counts = nullptr;
    const std::unordered_map<std::string, std::vector<std::string>>* zoning_group_zones = nullptr;
    const std::vector<std::string>* zoning_group_order = nullptr;
};

bool drawZoningFiltersPanel(ZoningFiltersPanelContext& ctx);
