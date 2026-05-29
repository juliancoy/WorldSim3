#pragma once

#include "imgui.h"
#include "cache_io.h"
#include "layer_runtime.h"
#include "types.h"

#include <cstddef>
#include <functional>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct ZoneMetadata;

struct MapHoverQuery {
    bool map_hovered = false;
    bool parcel_hover_active = false;
    bool parcel_inspect_active = false;
    bool zoning_hover_active = false;
    bool zoning_inspect_active = false;
    bool point_hover_active = false;
    bool point_inspect_active = false;
    int active_hover_layer_idx = -1;
    int active_click_layer_idx = -1;
    int parcel_layer_idx = -1;
    int zoning_layer_idx = -1;
    const std::vector<LayerDef>* layers = nullptr;
    const std::unordered_map<size_t, PointGeometryArtifact>* point_geometry_artifacts = nullptr;
    const std::unordered_map<size_t, PolygonGeometryArtifact>* polygon_geometry_artifacts = nullptr;
    const ParcelRenderCacheBlob* parcel_render_blob = nullptr;
    std::vector<LayerSpatialIndex>* layer_spatial = nullptr;
    const std::vector<bool>* layer_hover_enabled = nullptr;
    const std::vector<bool>* layer_inspect_enabled = nullptr;
    ImVec2 mouse_ll;
    ImVec2 mouse_screen;
    ImVec2 center_lonlat = ImVec2(0.0f, 0.0f);
    ImVec2 center_world = ImVec2(0.0f, 0.0f);
    ImVec2 viewport_origin = ImVec2(0.0f, 0.0f);
    ImVec2 viewport_size = ImVec2(0.0f, 0.0f);
    ImVec2 framebuffer_size = ImVec2(0.0f, 0.0f);
    float view_min_lon = 0.0f;
    float view_max_lon = 0.0f;
    float view_min_lat = 0.0f;
    float view_max_lat = 0.0f;
    int math_zoom = 0;
    float zoom_scale = 1.0f;
    std::function<ImVec2(const ImVec2&)> project_world;
};

struct MapHoverState {
    int hovered_parcel_layer_idx = -1;
    size_t hovered_parcel_idx = (size_t)-1;
    std::string hovered_parcel_entity_id;
    std::string hovered_parcel_geometry_entity_id;
    int inspect_parcel_layer_idx = -1;
    size_t inspect_parcel_idx = (size_t)-1;
    std::string inspect_parcel_entity_id;
    std::string inspect_parcel_geometry_entity_id;
    const LayerDef::FeatureRecord* hovered_zone = nullptr;
    size_t hovered_zone_idx = (size_t)-1;
    std::string hovered_zone_entity_id;
    const LayerDef::FeatureRecord* hovered_point = nullptr;
    size_t hovered_point_idx = (size_t)-1;
    int hovered_point_layer_idx = -1;
    const LayerDef::FeatureRecord* inspect_point = nullptr;
    size_t inspect_point_idx = (size_t)-1;
    int inspect_point_layer_idx = -1;
};

struct HoverDebugState {
    std::mutex mutex;
    bool map_hovered = false;
    float mouse_screen_x = 0.0f;
    float mouse_screen_y = 0.0f;
    float mouse_lon = 0.0f;
    float mouse_lat = 0.0f;
    bool hovered_parcel = false;
    int hovered_parcel_layer_idx = -1;
    size_t hovered_parcel_idx = (size_t)-1;
    std::string hovered_parcel_entity_id;
    std::string hovered_parcel_geometry_entity_id;
    bool inspect_parcel = false;
    int inspect_parcel_layer_idx = -1;
    size_t inspect_parcel_idx = (size_t)-1;
    std::string inspect_parcel_entity_id;
    std::string inspect_parcel_geometry_entity_id;
    bool hovered_zone = false;
    size_t hovered_zone_idx = (size_t)-1;
    bool hovered_point = false;
    size_t hovered_point_idx = (size_t)-1;
    int hovered_point_layer_idx = -1;
    bool selected_parcel = false;
    int selected_parcel_layer_idx = -1;
    size_t selected_parcel_idx = (size_t)-1;
    std::string selected_parcel_entity_id;
    std::string selected_parcel_geometry_entity_id;
    size_t selected_parcel_count = 0;
};

MapHoverState findMapHoverTargets(const MapHoverQuery& query);
uint64_t stablePointFeatureOrderKey(size_t layer_idx, size_t feature_idx, const LayerDef::FeatureRecord& fg);
void drawZoningHoverTooltip(
    const LayerDef::FeatureRecord& zone,
    const std::unordered_map<std::string, ZoneMetadata>& zoning_metadata);
