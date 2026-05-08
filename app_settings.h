#pragma once

#include <filesystem>

struct AppSettings {
    bool vulkan_validation_enabled = false;
    bool grayscale_basemap = false;
    int basemap_style = 0; // 0=OpenStreetMap, 1=Topographic
    bool topo_vector_enabled = false;
    bool zoning_use_simcity_colors = true;
    int reserve_cpu_cores = 0;
};

AppSettings loadAppSettings(const std::filesystem::path& root, const AppSettings& defaults);
void saveAppSettings(const std::filesystem::path& root, const AppSettings& settings);
