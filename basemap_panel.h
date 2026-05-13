#pragma once

#include "app_settings.h"
#include "download_queue.h"
#include "tiles.h"

#include <cstddef>
#include <filesystem>
#include <string>

struct BasemapPanelContext {
    const std::filesystem::path* root = nullptr;
    AppSettings* app_settings = nullptr;
    DownloadQueueState* basemap_download = nullptr;
    LazyTileDownloadState* lazy_tile_download = nullptr;
    std::string* data_library_status_msg = nullptr;
    bool* basemap_coverage_dirty = nullptr;
    size_t osm_missing_tiles_cached = 0;
    size_t osm_total_tiles_cached = 0;
    size_t topo_missing_tiles_cached = 0;
    size_t topo_total_tiles_cached = 0;
    bool topo_tiles_available_cached = false;
    bool topo_vector_available_cached = false;
    int min_zoom = 0;
    int max_native_tile_zoom = 0;
    int max_satellite_native_tile_zoom = 0;
};

void drawBasemapPanel(BasemapPanelContext& ctx);
