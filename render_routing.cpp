#include "render_routing.h"

#include "app_utils.h"

bool isOperationalParcelRenderLayer(const LayerDef& layer) {
    return layer.scale == "parcel" && layer.duckdb_role == "parcel_record";
}

LayerRenderRoute classifyLayerRenderRoute(size_t layer_idx, const LayerDef& layer, int active_parcel_layer_idx) {
    (void)layer_idx;
    (void)active_parcel_layer_idx;
    if (isOperationalParcelRenderLayer(layer)) {
        return LayerRenderRoute::ParcelPolygonGpu;
    }
    if (layerUsesPointGeometry(layer)) return LayerRenderRoute::PointGpu;
    if (layerUsesPolylineGeometry(layer)) return LayerRenderRoute::PolylineGpu;
    return LayerRenderRoute::GenericPolygonGpu;
}

const char* layerRenderRouteName(LayerRenderRoute route, bool gpu_resident) {
    switch (route) {
        case LayerRenderRoute::ParcelGpu:
            return gpu_resident ? "parcel_gpu" : "parcel_gpu_pending";
        case LayerRenderRoute::ParcelPolygonGpu:
            return gpu_resident ? "parcel_polygon_gpu" : "parcel_polygon_gpu_pending";
        case LayerRenderRoute::PointGpu:
            return gpu_resident ? "point_gpu" : "point_gpu_pending";
        case LayerRenderRoute::PolylineGpu:
            return gpu_resident ? "polyline_gpu" : "polyline_gpu_pending";
        case LayerRenderRoute::GenericPolygonGpu:
            return gpu_resident ? "generic_polygon_gpu" : "generic_polygon_gpu_pending";
    }
    return "unknown";
}

const char* layerRenderRouteArtifactName(LayerRenderRoute route) {
    switch (route) {
        case LayerRenderRoute::ParcelGpu:
            return "parcel_gpu";
        case LayerRenderRoute::ParcelPolygonGpu:
            return "parcel_polygon_gpu";
        case LayerRenderRoute::PointGpu:
            return "point_gpu";
        case LayerRenderRoute::PolylineGpu:
            return "polyline_gpu";
        case LayerRenderRoute::GenericPolygonGpu:
            return "generic_polygon_gpu";
    }
    return "unknown";
}

const char* layerRenderRouteReason(LayerRenderRoute route) {
    switch (route) {
        case LayerRenderRoute::ParcelGpu:
            return "legacy monolithic parcel GPU path";
        case LayerRenderRoute::ParcelPolygonGpu:
            return "operational parcel layer rendered through parcel polygon GPU path";
        case LayerRenderRoute::PointGpu:
            return "point geometry layer";
        case LayerRenderRoute::PolylineGpu:
            return "polyline geometry layer";
        case LayerRenderRoute::GenericPolygonGpu:
            return "non-parcel polygon layer";
    }
    return "unknown render route";
}
