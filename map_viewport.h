#pragma once

#include "imgui.h"

#include <functional>

struct MapViewportContext {
    double* center_lon = nullptr;
    double* center_lat = nullptr;
    int* zoom = nullptr;
    int min_zoom = 0;
    int max_zoom = 0;
    int max_internal_math_zoom = 0;
    bool dark_mode = false;
};

struct MapViewportFrame {
    ImDrawList* draw = nullptr;
    ImVec2 origin;
    ImVec2 size;
    bool hovered = false;
    bool active = false;
    int math_zoom = 0;
    double zoom_scale = 1.0;
    ImVec2 center_world;
    ImVec2 mouse_ll;
    float view_min_lon = 0.0f;
    float view_max_lon = 0.0f;
    float view_min_lat = 0.0f;
    float view_max_lat = 0.0f;

    ImVec2 projectWorld(const ImVec2& world_px) const;
};

MapViewportFrame beginMapViewportCanvas(const MapViewportContext& ctx);
