#include "gpu_picking.h"

#include "aggregate_debug.h"
#include "app_utils.h"
#include "worldsim_app.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {
void logParcelPickLine(const std::string& line) {
    static std::string last_line;
    if (line == last_line) return;
    last_line = line;
    std::fprintf(stderr, "%s\n", line.c_str());
}

void logParcelHoverStateChange(
    const char* stage,
    bool hover_target,
    size_t layer_idx,
    size_t feature_ref,
    size_t feature_idx,
    const std::string& entity_id,
    const std::string& detail = {}) {
    if (!worldsimParcelPickDebugEnabled()) return;
    struct LastState {
        std::string stage;
        bool hover_target = false;
        size_t layer_idx = (size_t)-1;
        size_t feature_ref = (size_t)-1;
        size_t feature_idx = (size_t)-1;
        std::string entity_id;
        std::string detail;
    };
    static LastState last;
    LastState current;
    current.stage = stage ? stage : "unknown";
    current.hover_target = hover_target;
    current.layer_idx = layer_idx;
    current.feature_ref = feature_ref;
    current.feature_idx = feature_idx;
    current.entity_id = entity_id;
    current.detail = detail;
    if (current.stage == last.stage &&
        current.hover_target == last.hover_target &&
        current.layer_idx == last.layer_idx &&
        current.feature_ref == last.feature_ref &&
        current.feature_idx == last.feature_idx &&
        current.entity_id == last.entity_id &&
        current.detail == last.detail) {
        return;
    }
    last = current;

    char buffer[1024];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "[worldsim3][parcel-pick] stage=%s target=%s layer=%zu feature_ref=%zu feature=%zu entity=%s detail=%s",
        current.stage.c_str(),
        hover_target ? "hover" : "inspect",
        layer_idx,
        feature_ref,
        feature_idx,
        entity_id.c_str(),
        detail.c_str());
    logParcelPickLine(buffer);
}

void logParcelPickTrace(
    const char* stage,
    bool hover_target,
    size_t layer_idx,
    size_t feature_idx,
    const std::string& entity_id,
    const std::string& detail = {}) {
    logParcelHoverStateChange(stage, hover_target, layer_idx, (size_t)-1, feature_idx, entity_id, detail);
}

void logParcelPickResolved(
    const char* stage,
    bool hover_target,
    size_t layer_idx,
    size_t feature_ref,
    size_t feature_idx,
    const std::string& entity_id,
    const std::string& detail = {}) {
    logParcelHoverStateChange(stage, hover_target, layer_idx, feature_ref, feature_idx, entity_id, detail);
}

void logParcelPickState(
    const char* stage,
    bool hover_target,
    int active_layer_idx,
    int parcel_layer_idx,
    bool map_hovered,
    bool parcel_target_active,
    float mouse_lon = 0.0f,
    float mouse_lat = 0.0f,
    size_t candidates = 0) {
    char detail[256];
    std::snprintf(
        detail,
        sizeof(detail),
        "candidates=%zu active_layer=%d parcel_layer=%d map_hovered=%d parcel_active=%d mouse_ll=%.7f,%.7f",
        candidates,
        active_layer_idx,
        parcel_layer_idx,
        map_hovered ? 1 : 0,
        parcel_target_active ? 1 : 0,
        mouse_lon,
        mouse_lat);
    logParcelHoverStateChange(stage, hover_target, (size_t)-1, (size_t)-1, (size_t)-1, {}, detail);
}

GpuPickRequest makePickRequest(const MapHoverQuery& query) {
    GpuPickRequest request;
    request.math_zoom = query.math_zoom;
    request.zoom_scale = query.zoom_scale;
    request.center_lonlat = query.center_lonlat;
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

std::vector<size_t> parcelPickCandidateLayers(
    const MapHoverQuery& query,
    bool hover_target,
    const std::vector<bool>* enabled_flags,
    int active_layer_idx) {
    std::vector<size_t> out;
    if (!query.layers || !enabled_flags) return out;
    const size_t count = std::min(query.layers->size(), enabled_flags->size());
    auto append_if_parcel_pickable = [&](int candidate_idx, bool require_target_flag) {
        if (candidate_idx < 0) return;
        const size_t idx = (size_t)candidate_idx;
        if (idx >= count) return;
        if (require_target_flag && !(*enabled_flags)[idx]) return;
        const LayerDef& layer = (*query.layers)[idx];
        if (!layer.enabled || !isParcelInteractionLayer(layer)) return;
        if (std::find(out.begin(), out.end(), idx) == out.end()) out.push_back(idx);
    };

    append_if_parcel_pickable(active_layer_idx, true);
    append_if_parcel_pickable(query.parcel_layer_idx, false);

    // Parcel interaction is semantic-class based. Once parcel inspect/hover mode
    // is active, all enabled operational parcel layers are valid GPU pick targets.
    // For click-time parcel selection, active_layer_idx may be -1 when the user
    // has no explicit click target selected; visible parcel layers remain valid.
    for (size_t i = count; i-- > 0;) {
        append_if_parcel_pickable((int)i, false);
    }
    return out;
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

void tryPickParcel(
    const MapHoverQuery& query,
    bool hover_target,
    size_t* out_feature_idx,
    int* out_layer_idx,
    std::string* out_entity_id,
    std::string* out_geometry_entity_id) {
    const bool parcel_target_active = hover_target ? query.parcel_hover_active : query.parcel_inspect_active;
    if (!query.map_hovered || !parcel_target_active || !query.layers ||
        !out_feature_idx || !out_layer_idx || !out_entity_id || !out_geometry_entity_id) {
        return;
    }
    const auto* enabled_flags = hover_target ? query.layer_hover_enabled : query.layer_inspect_enabled;
    const int active_layer_idx = hover_target ? query.active_hover_layer_idx : query.active_click_layer_idx;
    const std::vector<size_t> candidate_layers =
        parcelPickCandidateLayers(query, hover_target, enabled_flags, active_layer_idx);
    if (candidate_layers.empty()) {
        logParcelPickState(
            "no-candidates",
            hover_target,
            active_layer_idx,
            query.parcel_layer_idx,
            query.map_hovered,
            parcel_target_active);
        return;
    }

    const GpuPickRequest request = makePickRequest(query);
    for (const size_t layer_idx : candidate_layers) {
        const LayerDef& layer = (*query.layers)[layer_idx];
        size_t feature_idx = (size_t)-1;
        size_t raw_feature_idx = (size_t)-1;
        std::string entity_id;
        std::string geometry_entity_id;
        std::string pick_error;
        const bool use_semantic_parcel_gpu = query.parcel_layer_idx >= 0 && layer_idx == (size_t)query.parcel_layer_idx;
        const bool gpu_ok = use_semantic_parcel_gpu
            ? gpuPickParcelFeature(request, &feature_idx, &entity_id, &geometry_entity_id, &pick_error)
            : gpuPickZoningFeature(layer_idx, request, &feature_idx, &entity_id, &geometry_entity_id, &pick_error);
        raw_feature_idx = feature_idx;
        if (!gpu_ok) {
            logParcelPickTrace("gpu-failed", hover_target, layer_idx, feature_idx, entity_id, pick_error);
            continue;
        } else if (feature_idx == (size_t)-1) {
            logParcelPickTrace("gpu-empty", hover_target, layer_idx, feature_idx, entity_id, "parcel polygon gpu pick returned no feature");
            continue;
        }
        if (feature_idx >= layer.features.size() && entity_id.empty()) {
            logParcelPickTrace("out-of-range", hover_target, layer_idx, feature_idx, entity_id, "layer feature index out of range");
            continue;
        }
        *out_layer_idx = (int)layer_idx;
        *out_feature_idx = feature_idx;
        *out_entity_id = std::move(entity_id);
        *out_geometry_entity_id = std::move(geometry_entity_id);
        logParcelPickResolved(
            feature_idx < layer.features.size() ? "resolved-gpu" : "resolved-gpu-entity",
            hover_target,
            layer_idx,
            raw_feature_idx,
            feature_idx,
            *out_entity_id,
            use_semantic_parcel_gpu ? "parcel_gpu" : layer.file);
        return;
    }
    logParcelPickState(
        "miss",
        hover_target,
        active_layer_idx,
        query.parcel_layer_idx,
        query.map_hovered,
        parcel_target_active,
        query.mouse_ll.x,
        query.mouse_ll.y,
        candidate_layers.size());
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
    std::string geometry_entity_id;
    std::string pick_error;
    if (!gpuPickZoningFeature(layer_idx, makePickRequest(query), &feature_idx, &entity_id, &geometry_entity_id, &pick_error)) {
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
    // Point hover should not suppress parcel inspection; click handling decides precedence later.
    tryPickParcel(
        query,
        true,
        &out.hovered_parcel_idx,
        &out.hovered_parcel_layer_idx,
        &out.hovered_parcel_entity_id,
        &out.hovered_parcel_geometry_entity_id);
    tryPickParcel(
        query,
        false,
        &out.inspect_parcel_idx,
        &out.inspect_parcel_layer_idx,
        &out.inspect_parcel_entity_id,
        &out.inspect_parcel_geometry_entity_id);
    if (out.hovered_parcel_idx != (size_t)-1) return out;
    tryPickZone(query, out);
    return out;
}
