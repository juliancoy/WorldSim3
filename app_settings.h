#pragma once

#include <filesystem>

struct AppSettings {
    bool dark_mode = false;
    bool vulkan_validation_enabled = false;
    bool grayscale_basemap = false;
    bool basemap_osm_enabled = true;
    bool basemap_topographic_enabled = false;
    bool basemap_satellite_enabled = false;
    bool basemap_dark_satellite_enabled = false;
    bool basemap_night_satellite_enabled = false;
    float basemap_osm_opacity = 1.0f;
    float basemap_topographic_opacity = 1.0f;
    float basemap_satellite_opacity = 1.0f;
    float basemap_dark_satellite_opacity = 1.0f;
    float basemap_night_satellite_opacity = 1.0f;
    float map_polygon_fill_opacity = 170.0f / 255.0f;
    bool topo_vector_enabled = false;
    bool zoning_use_simcity_colors = true;
    int reserve_cpu_cores = 0;
};

AppSettings loadAppSettings(const std::filesystem::path& root, const AppSettings& defaults);
void saveAppSettings(const std::filesystem::path& root, const AppSettings& settings);
