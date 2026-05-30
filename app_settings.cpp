#include "app_settings.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

AppSettings loadAppSettings(const fs::path& root, const AppSettings& defaults) {
    std::ifstream in(root / "data" / "app_settings.json");
    if (!in) return defaults;
    AppSettings out = defaults;
    json j;
    try {
        in >> j;
    } catch (...) {
        return defaults;
    }
    if (j.contains("dark_mode") && j["dark_mode"].is_boolean()) {
        out.dark_mode = j["dark_mode"].get<bool>();
    }
    if (j.contains("vulkan_validation_enabled") && j["vulkan_validation_enabled"].is_boolean()) {
        out.vulkan_validation_enabled = j["vulkan_validation_enabled"].get<bool>();
    }
    if (j.contains("grayscale_basemap") && j["grayscale_basemap"].is_boolean()) {
        out.grayscale_basemap = j["grayscale_basemap"].get<bool>();
    }
    const bool has_independent_basemaps =
        j.contains("basemap_osm_enabled") ||
        j.contains("basemap_topographic_enabled") ||
        j.contains("basemap_satellite_enabled") ||
        j.contains("basemap_dark_satellite_enabled") ||
        j.contains("basemap_night_satellite_enabled");
    if (has_independent_basemaps && j.contains("basemap_osm_enabled") && j["basemap_osm_enabled"].is_boolean()) {
        out.basemap_osm_enabled = j["basemap_osm_enabled"].get<bool>();
    }
    if (has_independent_basemaps && j.contains("basemap_topographic_enabled") && j["basemap_topographic_enabled"].is_boolean()) {
        out.basemap_topographic_enabled = j["basemap_topographic_enabled"].get<bool>();
    }
    if (has_independent_basemaps && j.contains("basemap_satellite_enabled") && j["basemap_satellite_enabled"].is_boolean()) {
        out.basemap_satellite_enabled = j["basemap_satellite_enabled"].get<bool>();
    }
    if (has_independent_basemaps && j.contains("basemap_dark_satellite_enabled") && j["basemap_dark_satellite_enabled"].is_boolean()) {
        out.basemap_dark_satellite_enabled = j["basemap_dark_satellite_enabled"].get<bool>();
    }
    if (has_independent_basemaps && j.contains("basemap_night_satellite_enabled") && j["basemap_night_satellite_enabled"].is_boolean()) {
        out.basemap_night_satellite_enabled = j["basemap_night_satellite_enabled"].get<bool>();
    }
    if (j.contains("basemap_osm_opacity") && j["basemap_osm_opacity"].is_number()) {
        out.basemap_osm_opacity = std::clamp(j["basemap_osm_opacity"].get<float>(), 0.0f, 1.0f);
    }
    if (j.contains("basemap_topographic_opacity") && j["basemap_topographic_opacity"].is_number()) {
        out.basemap_topographic_opacity = std::clamp(j["basemap_topographic_opacity"].get<float>(), 0.0f, 1.0f);
    }
    if (j.contains("basemap_satellite_opacity") && j["basemap_satellite_opacity"].is_number()) {
        out.basemap_satellite_opacity = std::clamp(j["basemap_satellite_opacity"].get<float>(), 0.0f, 1.0f);
    }
    if (j.contains("basemap_dark_satellite_opacity") && j["basemap_dark_satellite_opacity"].is_number()) {
        out.basemap_dark_satellite_opacity = std::clamp(j["basemap_dark_satellite_opacity"].get<float>(), 0.0f, 1.0f);
    }
    if (j.contains("basemap_night_satellite_opacity") && j["basemap_night_satellite_opacity"].is_number()) {
        out.basemap_night_satellite_opacity = std::clamp(j["basemap_night_satellite_opacity"].get<float>(), 0.0f, 1.0f);
    }
    if (j.contains("map_polygon_fill_opacity") && j["map_polygon_fill_opacity"].is_number()) {
        out.map_polygon_fill_opacity = std::clamp(j["map_polygon_fill_opacity"].get<float>(), 0.0f, 1.0f);
    }
    if (j.contains("map_polygon_outline_thickness") && j["map_polygon_outline_thickness"].is_number()) {
        out.map_polygon_outline_thickness = std::clamp(j["map_polygon_outline_thickness"].get<float>(), 1.0f, 8.0f);
    }
    if (j.contains("zoom_step") && j["zoom_step"].is_number()) {
        out.zoom_step = std::clamp(j["zoom_step"].get<double>(), 0.05, 4.0);
    }
    if (!out.basemap_osm_enabled && !out.basemap_topographic_enabled &&
        !out.basemap_satellite_enabled && !out.basemap_dark_satellite_enabled &&
        !out.basemap_night_satellite_enabled) {
        out.basemap_osm_enabled = true;
    }
    if (j.contains("topo_vector_enabled") && j["topo_vector_enabled"].is_boolean()) {
        out.topo_vector_enabled = j["topo_vector_enabled"].get<bool>();
    }
    if (j.contains("zoning_use_simcity_colors") && j["zoning_use_simcity_colors"].is_boolean()) {
        out.zoning_use_simcity_colors = j["zoning_use_simcity_colors"].get<bool>();
    }
    if (j.contains("reserve_cpu_cores") && j["reserve_cpu_cores"].is_number_integer()) {
        out.reserve_cpu_cores = std::max(0, j["reserve_cpu_cores"].get<int>());
    }
    if (j.contains("map_title_text") && j["map_title_text"].is_string()) {
        out.map_title_text = j["map_title_text"].get<std::string>();
    }
    if (j.contains("map_title_show_primary_parcel_source") && j["map_title_show_primary_parcel_source"].is_boolean()) {
        out.map_title_show_primary_parcel_source = j["map_title_show_primary_parcel_source"].get<bool>();
    }
    if (j.contains("map_title_all_caps") && j["map_title_all_caps"].is_boolean()) {
        out.map_title_all_caps = j["map_title_all_caps"].get<bool>();
    }
    if (j.contains("map_legend_show_overlay") && j["map_legend_show_overlay"].is_boolean()) {
        out.map_legend_show_overlay = j["map_legend_show_overlay"].get<bool>();
    }
    if (j.contains("map_legend_overlay_position") && j["map_legend_overlay_position"].is_number_integer()) {
        out.map_legend_overlay_position = std::clamp(j["map_legend_overlay_position"].get<int>(), 0, 3);
    }
    return out;
}

void saveAppSettings(const fs::path& root, const AppSettings& settings) {
    fs::create_directories(root / "data");
    json j;
    j["dark_mode"] = settings.dark_mode;
    j["vulkan_validation_enabled"] = settings.vulkan_validation_enabled;
    j["grayscale_basemap"] = settings.grayscale_basemap;
    j["basemap_osm_enabled"] = settings.basemap_osm_enabled;
    j["basemap_topographic_enabled"] = settings.basemap_topographic_enabled;
    j["basemap_satellite_enabled"] = settings.basemap_satellite_enabled;
    j["basemap_dark_satellite_enabled"] = settings.basemap_dark_satellite_enabled;
    j["basemap_night_satellite_enabled"] = settings.basemap_night_satellite_enabled;
    j["basemap_osm_opacity"] = std::clamp(settings.basemap_osm_opacity, 0.0f, 1.0f);
    j["basemap_topographic_opacity"] = std::clamp(settings.basemap_topographic_opacity, 0.0f, 1.0f);
    j["basemap_satellite_opacity"] = std::clamp(settings.basemap_satellite_opacity, 0.0f, 1.0f);
    j["basemap_dark_satellite_opacity"] = std::clamp(settings.basemap_dark_satellite_opacity, 0.0f, 1.0f);
    j["basemap_night_satellite_opacity"] = std::clamp(settings.basemap_night_satellite_opacity, 0.0f, 1.0f);
    j["map_polygon_fill_opacity"] = std::clamp(settings.map_polygon_fill_opacity, 0.0f, 1.0f);
    j["map_polygon_outline_thickness"] = std::clamp(settings.map_polygon_outline_thickness, 1.0f, 8.0f);
    j["zoom_step"] = std::clamp(settings.zoom_step, 0.05, 4.0);
    j["topo_vector_enabled"] = settings.topo_vector_enabled;
    j["zoning_use_simcity_colors"] = settings.zoning_use_simcity_colors;
    j["reserve_cpu_cores"] = std::max(0, settings.reserve_cpu_cores);
    j["map_title_text"] = settings.map_title_text;
    j["map_title_show_primary_parcel_source"] = settings.map_title_show_primary_parcel_source;
    j["map_title_all_caps"] = settings.map_title_all_caps;
    j["map_legend_show_overlay"] = settings.map_legend_show_overlay;
    j["map_legend_overlay_position"] = std::clamp(settings.map_legend_overlay_position, 0, 3);
    std::ofstream out(root / "data" / "app_settings.json");
    if (out) out << j.dump(2);
}
