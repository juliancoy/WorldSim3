#pragma once

#include "cache_io.h"
#include "imgui.h"
#include "map_render_projection.h"
#include "selection.h"
#include "types.h"

#include <cstddef>
#include <functional>
#include <vector>

struct MapSelectionRenderContext {
    ImDrawList* draw = nullptr;
    ImVec2 origin;
    ImVec2 size;
    const std::vector<LayerDef>* layers = nullptr;
    const std::unordered_map<size_t, PolygonGeometryArtifact>* polygon_geometry_artifacts = nullptr;
    const ParcelRenderCacheBlob* parcel_render_blob = nullptr;
    int parcel_layer_idx = -1;
    int zoning_layer_idx = -1;
    const ParcelSelectionState* parcel_selection = nullptr;
    const bool* show_selected_zone_details = nullptr;
    const size_t* selected_zone_idx = nullptr;
    int math_zoom = 0;
    MapProjectionCache* projection = nullptr;
    std::function<ImVec2(const ImVec2&)> project_world;
};

void renderSelectedParcelOutlines(const MapSelectionRenderContext& ctx);
void renderSelectedZoneOutline(const MapSelectionRenderContext& ctx);
