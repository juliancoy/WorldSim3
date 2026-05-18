#pragma once

#include "imgui.h"
#include "layer_runtime.h"
#include "types.h"

#include <cstddef>
#include <functional>
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

MapHoverState findMapHoverTargets(const MapHoverQuery& query);
void drawZoningHoverTooltip(
    const LayerDef::FeatureGeom& zone,
    const std::unordered_map<std::string, ZoneMetadata>& zoning_metadata);
