#pragma once

#include "app_settings.h"
#include "imgui.h"
#include "layer_runtime.h"
#include "map_render_hover.h"
#include "map_render_projection.h"
#include "map_viewport.h"
#include "tiles.h"
#include "types.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct MapCanvasSessionContext {
    double* center_lon = nullptr;
    double* center_lat = nullptr;
    int* zoom = nullptr;
    int min_zoom = 0;
    int max_zoom = 0;
    int max_internal_math_zoom = 0;

    const std::filesystem::path* root = nullptr;
    const AppSettings* app_settings = nullptr;
    LazyTileDownloadState* lazy_tile_download = nullptr;
    bool* topo_tiles_available_cached = nullptr;
    bool* topo_vector_available_cached = nullptr;
    bool* basemap_source_has_any_files_cached = nullptr;
    std::string* tile_root_dir_cached = nullptr;
    std::chrono::steady_clock::time_point* basemap_availability_last_check = nullptr;
    int max_native_tile_zoom = 18;

    std::vector<LayerDef>* layers = nullptr;
    std::vector<LayerSpatialIndex>* layer_spatial = nullptr;
    const std::vector<bool>* layer_hover_enabled = nullptr;
    const std::vector<bool>* layer_inspect_enabled = nullptr;

    int hover_inspector_mode = 0;
    bool* hover_inspector_enabled = nullptr;
    int parcel_layer_idx = -1;
    int zoning_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;

    size_t* prof_tiles_drawn_frame = nullptr;
    std::atomic<double>* prof_tile_ms_last = nullptr;
    std::unique_ptr<MapProjectionCache>* persistent_projection_cache = nullptr;
    size_t* persistent_projection_generation = nullptr;
    size_t projection_generation = 0;
    std::atomic<size_t>* prof_projection_world_ring_cache_entries = nullptr;
    std::atomic<size_t>* prof_projection_world_extent_cache_entries = nullptr;
    std::atomic<size_t>* prof_projection_cache_generation = nullptr;
};

struct MapCanvasSession {
    ImDrawList* draw = nullptr;
    ImVec2 origin = ImVec2(0.0f, 0.0f);
    ImVec2 size = ImVec2(0.0f, 0.0f);
    bool map_hovered = false;
    bool map_active = false;
    int math_zoom = 0;
    double zoom_scale = 1.0;
    ImVec2 center_world = ImVec2(0.0f, 0.0f);
    float view_min_lon = 0.0f;
    float view_max_lon = 0.0f;
    float view_min_lat = 0.0f;
    float view_max_lat = 0.0f;
    bool parcel_hover_active = false;
    bool parcel_inspect_active = false;
    bool zoning_hover_active = false;
    bool zoning_inspect_active = false;
    MapHoverState hover_state;
    bool vacant_notice_enabled = false;
    bool vacant_rehab_enabled = false;
    bool tax_lien_enabled = false;
    bool tax_sale_enabled = false;
    ImVec4 vacancy_notice_color = ImVec4(1, 0, 0, 1);
    ImVec4 vacancy_rehab_color = ImVec4(0, 1, 1, 1);
    int lod_ring_step = 1;
    std::function<ImVec2(const ImVec2&)> project_world;
    std::function<bool(size_t)> should_fill_layer_polygon;
    MapProjectionCache* projection_cache = nullptr;
};

MapCanvasSession beginMapCanvasSession(const MapCanvasSessionContext& ctx);
