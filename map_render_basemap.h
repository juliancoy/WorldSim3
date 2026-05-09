#pragma once

#include "app_settings.h"
#include "imgui.h"
#include "tiles.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>

struct MapBasemapRenderContext {
    ImDrawList* draw = nullptr;
    ImVec2 origin;
    ImVec2 size;
    ImVec2 center_world;
    int math_zoom = 0;
    double zoom_scale = 1.0;
    const std::filesystem::path* root = nullptr;
    const AppSettings* app_settings = nullptr;
    LazyTileDownloadState* lazy_tile_download = nullptr;
    bool* topo_tiles_available_cached = nullptr;
    bool* topo_vector_available_cached = nullptr;
    bool* basemap_source_has_any_files_cached = nullptr;
    std::string* tile_root_dir_cached = nullptr;
    std::chrono::steady_clock::time_point* basemap_availability_last_check = nullptr;
    int max_native_tile_zoom = 18;
    std::function<ImVec2(const ImVec2&)> project_world;
};

struct MapBasemapRenderResult {
    size_t tiles_drawn = 0;
};

MapBasemapRenderResult renderMapBasemap(const MapBasemapRenderContext& ctx);
