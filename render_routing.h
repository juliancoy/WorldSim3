#pragma once

#include "types.h"

#include <cstddef>
#include <string>

enum class LayerRenderRoute {
    ParcelGpu,
    ParcelPolygonGpu,
    PointGpu,
    PolylineGpu,
    GenericPolygonGpu
};

bool isOperationalParcelRenderLayer(const LayerDef& layer);
LayerRenderRoute classifyLayerRenderRoute(size_t layer_idx, const LayerDef& layer, int active_parcel_layer_idx);
const char* layerRenderRouteName(LayerRenderRoute route, bool gpu_resident);
const char* layerRenderRouteArtifactName(LayerRenderRoute route);
const char* layerRenderRouteReason(LayerRenderRoute route);
