#include "gpu_picking.h"

#include "app_utils.h"
#include "worldsim_app.h"

namespace {
GpuPickRequest makePickRequest(const MapHoverQuery& query) {
    GpuPickRequest request;
    request.math_zoom = query.math_zoom;
    request.zoom_scale = query.zoom_scale;
    request.center_world = query.center_world;
    request.viewport_origin = query.viewport_origin;
    request.viewport_size = query.viewport_size;
    request.framebuffer_size = query.framebuffer_size;
    request.mouse_screen = query.mouse_screen;
    request.marker_radius_px = 5.0f;
    return request;
}

bool isParcelInteractionLayer(const LayerDef& layer) {
    return layer.scale == "parcel" &&
           !layerUsesPointGeometry(layer) &&
           !layerUsesPolylineGeometry(layer);
}

void tryPickPointForLayer(
    const MapHoverQuery& query,
    bool hover_target,
    size_t* out_feature_idx,
    int* out_layer_idx,
    const LayerDef::FeatureRecord** out_feature) {
    const bool point_target_active = hover_target ? query.point_hover_active : query.point_inspect_active;
    const auto* enabled_flags = hover_target ? query.layer_hover_enabled : query.layer_inspect_enabled;
    const int active_layer_idx = hover_target ? query.active_hover_layer_idx : query.active_click_layer_idx;
    if (!query.map_hovered || !point_target_active || !query.layers || !query.point_geometry_artifacts ||
        !enabled_flags || active_layer_idx < 0) {
        return;
    }
    const size_t layer_idx = (size_t)active_layer_idx;
    if (layer_idx >= query.layers->size() || layer_idx >= enabled_flags->size()) return;
    if (!(*enabled_flags)[layer_idx]) return;
    const LayerDef& layer = (*query.layers)[layer_idx];
    if (!layer.enabled || !layerUsesPointGeometry(layer)) return;
    auto artifact_it = query.point_geometry_artifacts->find(layer_idx);
    if (artifact_it == query.point_geometry_artifacts->end()) return;

    size_t feature_idx = (size_t)-1;
    std::string pick_error;
    if (!gpuPickPointFeature(
            artifact_it->second,
            artifact_it->second.header.source_signature,
            makePickRequest(query),
            &feature_idx,
            &pick_error)) {
        return;
    }
    if (feature_idx >= layer.features.size()) return;
    if (out_feature_idx) *out_feature_idx = feature_idx;
    if (out_layer_idx) *out_layer_idx = (int)layer_idx;
    if (out_feature) *out_feature = &layer.features[feature_idx];
}

void tryPickParcel(const MapHoverQuery& query, MapHoverState& out) {
    if (!query.map_hovered || (!query.parcel_hover_active && !query.parcel_inspect_active) ||
        !query.layers) {
        return;
    }
    const bool hover_target = query.parcel_hover_active;
    const auto* enabled_flags = hover_target ? query.layer_hover_enabled : query.layer_inspect_enabled;
    const int active_layer_idx = hover_target ? query.active_hover_layer_idx : query.active_click_layer_idx;
    std::vector<size_t> candidate_layers;
    candidate_layers.reserve(query.layers->size());
    auto append_candidate = [&](int layer_idx) {
        if (layer_idx < 0) return;
        const size_t idx = (size_t)layer_idx;
        if (idx >= query.layers->size()) return;
        if (std::find(candidate_layers.begin(), candidate_layers.end(), idx) != candidate_layers.end()) return;
        if (enabled_flags && idx < enabled_flags->size() && !(*enabled_flags)[idx]) return;
        const LayerDef& layer = (*query.layers)[idx];
        if (!layer.enabled || !isParcelInteractionLayer(layer)) return;
        candidate_layers.push_back(idx);
    };

    append_candidate(active_layer_idx);
    append_candidate(query.parcel_layer_idx);
    for (size_t layer_idx = 0; layer_idx < query.layers->size(); ++layer_idx) {
        append_candidate((int)layer_idx);
    }

    const GpuPickRequest request = makePickRequest(query);
    for (size_t layer_idx : candidate_layers) {
        const LayerDef& layer = (*query.layers)[layer_idx];
        size_t feature_idx = (size_t)-1;
        std::string entity_id;
        std::string pick_error;
        if ((int)layer_idx == query.parcel_layer_idx) {
            if (!gpuPickParcelFeature(request, &feature_idx, &entity_id, &pick_error)) {
                continue;
            }
            if (query.parcel_render_blob && feature_idx >= query.parcel_render_blob->features.size()) continue;
        } else {
            if (!gpuPickZoningFeature(layer_idx, request, &feature_idx, &entity_id, &pick_error)) {
                continue;
            }
        }
        if (feature_idx >= layer.features.size()) continue;
        out.hovered_parcel_layer_idx = (int)layer_idx;
        out.hovered_parcel_idx = feature_idx;
        out.hovered_parcel_entity_id = entity_id;
        return;
    }
}

void tryPickZone(const MapHoverQuery& query, MapHoverState& out) {
    if (!query.map_hovered || (!query.zoning_hover_active && !query.zoning_inspect_active) ||
        !query.layers || query.zoning_layer_idx < 0) {
        return;
    }
    const size_t layer_idx = (size_t)query.zoning_layer_idx;
    if (layer_idx >= query.layers->size()) return;
    const LayerDef& layer = (*query.layers)[layer_idx];
    if (!layer.enabled) return;

    size_t feature_idx = (size_t)-1;
    std::string entity_id;
    std::string pick_error;
    if (!gpuPickZoningFeature(layer_idx, makePickRequest(query), &feature_idx, &entity_id, &pick_error)) {
        return;
    }
    if (feature_idx >= layer.features.size()) return;
    out.hovered_zone_idx = feature_idx;
    out.hovered_zone = &layer.features[feature_idx];
    out.hovered_zone_entity_id = entity_id;
}
}

MapHoverState findGpuHoverTargets(const MapHoverQuery& query) {
    MapHoverState out;
    tryPickPointForLayer(
        query,
        true,
        &out.hovered_point_idx,
        &out.hovered_point_layer_idx,
        &out.hovered_point);
    tryPickPointForLayer(
        query,
        false,
        &out.inspect_point_idx,
        &out.inspect_point_layer_idx,
        &out.inspect_point);
    if (out.hovered_point) return out;
    tryPickParcel(query, out);
    if (out.hovered_parcel_idx != (size_t)-1) return out;
    tryPickZone(query, out);
    return out;
}
