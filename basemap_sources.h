#pragma once

#include <filesystem>
#include <string>

enum class BasemapSourceId {
    OpenStreetMap,
    Topographic,
    Satellite,
    DarkSatellite,
    NightLights
};

enum class BasemapLayerType {
    RasterTiles,
    VectorTopo,
    RasterTransform,
    NightLights
};

struct BasemapSourceDef {
    BasemapSourceId id;
    const char* label = "";
    const char* cache_dir = "";
    const char* url_template = "";
    int native_max_zoom = 0;
    int recommended_max_zoom = 0;
    BasemapLayerType layer_type = BasemapLayerType::RasterTiles;
    bool dark_mode_recommended = false;
    const char* notes = "";
};

const BasemapSourceDef& basemapSourceDef(BasemapSourceId id);
const char* basemapLayerTypeLabel(BasemapLayerType type);
std::string preferredTopoTileDir(const std::filesystem::path& root);
