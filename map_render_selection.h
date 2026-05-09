#pragma once

#include "imgui.h"
#include "map_render_projection.h"
#include "types.h"

#include <cstddef>
#include <functional>
#include <vector>

struct MapSelectionRenderContext {
    ImDrawList* draw = nullptr;
    ImVec2 origin;
    ImVec2 size;
    const std::vector<LayerDef>* layers = nullptr;
    int parcel_layer_idx = -1;
    const std::vector<size_t>* selected_parcel_indices = nullptr;
    int math_zoom = 0;
    MapProjectionCache* projection = nullptr;
    std::function<ImVec2(const ImVec2&)> project_world;
};

void renderSelectedParcelOutlines(const MapSelectionRenderContext& ctx);
