#include "basemap_sources.h"

#include "worldsim_app_internal.h"

#include <filesystem>

namespace fs = std::filesystem;

const BasemapSourceDef& basemapSourceDef(BasemapSourceId id) {
    static const BasemapSourceDef osm{
        BasemapSourceId::OpenStreetMap,
        "OpenStreetMap",
        "tiles",
        "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
        kMaxNativeTileZoom,
        kMaxZoom,
        BasemapLayerType::RasterTiles,
        false,
        "General-purpose raster basemap fetched lazily and cached on disk."
    };
    static const BasemapSourceDef topo{
        BasemapSourceId::Topographic,
        "Topographic",
        "tiles_topo",
        "https://services.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/{z}/{y}/{x}",
        kMaxNativeTileZoom,
        kMaxZoom,
        BasemapLayerType::VectorTopo,
        false,
        "Raster topo tiles with optional local vector contour representation."
    };
    static const BasemapSourceDef satellite{
        BasemapSourceId::Satellite,
        "Satellite",
        "tiles_satellite",
        "https://services.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
        kMaxSatelliteNativeTileZoom,
        kMaxZoom,
        BasemapLayerType::RasterTiles,
        false,
        "High-zoom daytime imagery."
    };
    static const BasemapSourceDef dark_satellite{
        BasemapSourceId::DarkSatellite,
        "Dark satellite",
        "tiles_satellite",
        "https://services.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
        kMaxSatelliteNativeTileZoom,
        kMaxZoom,
        BasemapLayerType::RasterTransform,
        true,
        "Dark-mode transform over the daytime satellite cache; preserves high-zoom detail."
    };
    static const BasemapSourceDef night_lights{
        BasemapSourceId::NightLights,
        "Night satellite",
        "tiles_satellite_night",
        "https://tiles.arcgis.com/tiles/P3ePLMYs2RVChkJx/arcgis/rest/services/Earth_at_Night_WM/MapServer/tile/{z}/{y}/{x}",
        kMaxNightSatelliteNativeTileZoom,
        10,
        BasemapLayerType::NightLights,
        true,
        "Low native-detail night-lights imagery, overzoomed for context."
    };

    switch (id) {
        case BasemapSourceId::OpenStreetMap: return osm;
        case BasemapSourceId::Topographic: return topo;
        case BasemapSourceId::Satellite: return satellite;
        case BasemapSourceId::DarkSatellite: return dark_satellite;
        case BasemapSourceId::NightLights: return night_lights;
    }
    return osm;
}

const char* basemapLayerTypeLabel(BasemapLayerType type) {
    switch (type) {
        case BasemapLayerType::RasterTiles: return "Raster tiles";
        case BasemapLayerType::VectorTopo: return "Raster/vector topo";
        case BasemapLayerType::RasterTransform: return "Raster transform";
        case BasemapLayerType::NightLights: return "Night lights";
    }
    return "Unknown";
}

std::string preferredTopoTileDir(const fs::path& root) {
    if (fs::exists(root / "data" / "tiles_topographic")) return "tiles_topographic";
    return basemapSourceDef(BasemapSourceId::Topographic).cache_dir;
}
