#pragma once

#include "imgui.h"
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
    int active_hover_layer_idx = -1;
    int parcel_layer_idx = -1;
    int zoning_layer_idx = -1;
    const std::vector<LayerDef>* layers = nullptr;
    std::vector<LayerSpatialIndex>* layer_spatial = nullptr;
    const std::vector<bool>* layer_hover_enabled = nullptr;
    ImVec2 mouse_ll;
    ImVec2 mouse_screen;
    ImVec2 viewport_size = ImVec2(0.0f, 0.0f);
    float view_min_lon = 0.0f;
    float view_max_lon = 0.0f;
    float view_min_lat = 0.0f;
    float view_max_lat = 0.0f;
    int math_zoom = 0;
    std::function<ImVec2(const ImVec2&)> project_world;
};

struct MapHoverState {
    const LayerDef::FeatureGeom* hovered_parcel = nullptr;
    size_t hovered_parcel_idx = (size_t)-1;
    const LayerDef::FeatureGeom* hovered_zone = nullptr;
    size_t hovered_zone_idx = (size_t)-1;
    const LayerDef::FeatureGeom* hovered_point = nullptr;
    size_t hovered_point_idx = (size_t)-1;
    int hovered_point_layer_idx = -1;
};

struct HoverDebugState {
    std::mutex mutex;
    bool map_hovered = false;
    float mouse_screen_x = 0.0f;
    float mouse_screen_y = 0.0f;
    float mouse_lon = 0.0f;
    float mouse_lat = 0.0f;
    bool hovered_parcel = false;
    size_t hovered_parcel_idx = (size_t)-1;
    bool hovered_zone = false;
    size_t hovered_zone_idx = (size_t)-1;
    bool hovered_point = false;
    size_t hovered_point_idx = (size_t)-1;
    int hovered_point_layer_idx = -1;
};

MapHoverState findMapHoverTargets(const MapHoverQuery& query);
uint64_t stablePointFeatureOrderKey(size_t layer_idx, size_t feature_idx, const LayerDef::FeatureGeom& fg);
void drawZoningHoverTooltip(
    const LayerDef::FeatureGeom& zone,
    const std::unordered_map<std::string, ZoneMetadata>& zoning_metadata);
