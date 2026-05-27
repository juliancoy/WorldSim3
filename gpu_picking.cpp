#include "gpu_picking.h"

#include "app_utils.h"
#include "worldsim_app.h"

#include <algorithm>

namespace {
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

bool pointInTriangleLonLat(float px, float py, const ImVec2& a, const ImVec2& b, const ImVec2& c) {
    auto cross = [](const ImVec2& u, const ImVec2& v, float x, float y) {
        return (v.x - u.x) * (y - u.y) - (v.y - u.y) * (x - u.x);
    };
    const float c1 = cross(a, b, px, py);
    const float c2 = cross(b, c, px, py);
    const float c3 = cross(c, a, px, py);
    const bool has_neg = c1 < 0.0f || c2 < 0.0f || c3 < 0.0f;
    const bool has_pos = c1 > 0.0f || c2 > 0.0f || c3 > 0.0f;
    return !(has_neg && has_pos);
}

const ParcelRenderFeatureRecord* parcelRenderFeatureForFeatureIdx(
    const ParcelRenderCacheBlob& blob,
    size_t feature_idx) {
    if (feature_idx < blob.features.size() && blob.features[feature_idx].feature_idx == feature_idx) {
        return &blob.features[feature_idx];
    }
    for (const ParcelRenderFeatureRecord& rec : blob.features) {
        if (rec.feature_idx == feature_idx) return &rec;
    }
    return nullptr;
}

bool pointInParcelRenderFeature(
    const ParcelRenderCacheBlob& blob,
    const ParcelRenderFeatureRecord& rec,
    float lon,
    float lat) {
    if (lon < rec.min_lon || lon > rec.max_lon || lat < rec.min_lat || lat > rec.max_lat) return false;
    const uint32_t end = rec.index_offset + rec.index_count;
    if (end > blob.indices.size()) return false;
    for (uint32_t i = rec.index_offset; i + 2 < end; i += 3) {
        const uint32_t ia = blob.indices[i + 0];
        const uint32_t ib = blob.indices[i + 1];
        const uint32_t ic = blob.indices[i + 2];
        if (ia >= blob.vertices.size() || ib >= blob.vertices.size() || ic >= blob.vertices.size()) continue;
        if (pointInTriangleLonLat(lon, lat, blob.vertices[ia], blob.vertices[ib], blob.vertices[ic])) {
            return true;
        }
    }
    return false;
}

bool pointInPolygonArtifactFeature(
    const PolygonGeometryArtifact& artifact,
    size_t feature_idx,
    float lon,
    float lat) {
    if (feature_idx >= artifact.features.size()) return false;
    const GeometryArtifactFeatureRecord& rec = artifact.features[feature_idx];
    if (lon < rec.min_lon || lon > rec.max_lon || lat < rec.min_lat || lat > rec.max_lat) return false;
    const uint32_t end = rec.index_offset + rec.index_count;
    if (end > artifact.fill_indices.size()) return false;
    for (uint32_t i = rec.index_offset; i + 2 < end; i += 3) {
        const uint32_t ia = artifact.fill_indices[i + 0];
        const uint32_t ib = artifact.fill_indices[i + 1];
        const uint32_t ic = artifact.fill_indices[i + 2];
        if (ia >= artifact.vertices.size() || ib >= artifact.vertices.size() || ic >= artifact.vertices.size()) continue;
        if (pointInTriangleLonLat(lon, lat, artifact.vertices[ia], artifact.vertices[ib], artifact.vertices[ic])) {
            return true;
        }
    }
    return false;
}

bool queryParcelSpatialCandidates(
    const MapHoverQuery& query,
    size_t layer_idx,
    std::vector<uint32_t>& candidates) {
    if (!query.layer_spatial || layer_idx >= query.layer_spatial->size()) return false;
    constexpr float kPickEpsilonDeg = 0.0000005f;
    return queryLayerSpatialIndex(
        (*query.layer_spatial)[layer_idx],
        query.mouse_ll.x - kPickEpsilonDeg,
        query.mouse_ll.y - kPickEpsilonDeg,
        query.mouse_ll.x + kPickEpsilonDeg,
        query.mouse_ll.y + kPickEpsilonDeg,
        candidates);
}

bool tryPickParcelCpuFallbackForLayer(
    const MapHoverQuery& query,
    size_t layer_idx,
    size_t* out_feature_idx,
    std::string* out_entity_id) {
    if (!query.layers || layer_idx >= query.layers->size() || !out_feature_idx || !out_entity_id) return false;
    const LayerDef& layer = (*query.layers)[layer_idx];
    std::vector<uint32_t> candidates;
    if (!queryParcelSpatialCandidates(query, layer_idx, candidates) || candidates.empty()) return false;

    if ((int)layer_idx == query.parcel_layer_idx && query.parcel_render_blob) {
        for (uint32_t candidate : candidates) {
            if ((size_t)candidate >= layer.features.size()) continue;
            const ParcelRenderFeatureRecord* rec =
                parcelRenderFeatureForFeatureIdx(*query.parcel_render_blob, (size_t)candidate);
            if (!rec) continue;
            if (!pointInParcelRenderFeature(*query.parcel_render_blob, *rec, query.mouse_ll.x, query.mouse_ll.y)) {
                continue;
            }
            *out_feature_idx = (size_t)candidate;
            *out_entity_id = !rec->entity_id.empty()
                ? rec->entity_id
                : featureEntityIdForLayerFeature(layer, layer.features[(size_t)candidate], (size_t)candidate);
            return true;
        }
        return false;
    }

    if (!query.polygon_geometry_artifacts) return false;
    auto artifact_it = query.polygon_geometry_artifacts->find(layer_idx);
    if (artifact_it == query.polygon_geometry_artifacts->end()) return false;
    const PolygonGeometryArtifact& artifact = artifact_it->second;
    for (uint32_t candidate : candidates) {
        if ((size_t)candidate >= layer.features.size()) continue;
        if (!pointInPolygonArtifactFeature(artifact, (size_t)candidate, query.mouse_ll.x, query.mouse_ll.y)) {
            continue;
        }
        *out_feature_idx = (size_t)candidate;
        *out_entity_id = featureEntityIdForLayerFeature(layer, layer.features[(size_t)candidate], (size_t)candidate);
        return true;
    }
    return false;
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
    std::string* out_entity_id) {
    const bool parcel_target_active = hover_target ? query.parcel_hover_active : query.parcel_inspect_active;
    if (!query.map_hovered || !parcel_target_active || !query.layers ||
        !out_feature_idx || !out_layer_idx || !out_entity_id) {
        return;
    }
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
                if (!tryPickParcelCpuFallbackForLayer(query, layer_idx, &feature_idx, &entity_id)) {
                    continue;
                }
            } else if (feature_idx == (size_t)-1 &&
                       !tryPickParcelCpuFallbackForLayer(query, layer_idx, &feature_idx, &entity_id)) {
                continue;
            }
            if (query.parcel_render_blob && feature_idx >= query.parcel_render_blob->features.size()) continue;
        } else {
            if (!gpuPickZoningFeature(layer_idx, request, &feature_idx, &entity_id, &pick_error)) {
                if (!tryPickParcelCpuFallbackForLayer(query, layer_idx, &feature_idx, &entity_id)) {
                    continue;
                }
            } else if (feature_idx == (size_t)-1 &&
                       !tryPickParcelCpuFallbackForLayer(query, layer_idx, &feature_idx, &entity_id)) {
                continue;
            }
        }
        if (feature_idx >= layer.features.size()) continue;
        *out_layer_idx = (int)layer_idx;
        *out_feature_idx = feature_idx;
        *out_entity_id = std::move(entity_id);
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
    tryPickParcel(
        query,
        true,
        &out.hovered_parcel_idx,
        &out.hovered_parcel_layer_idx,
        &out.hovered_parcel_entity_id);
    tryPickParcel(
        query,
        false,
        &out.inspect_parcel_idx,
        &out.inspect_parcel_layer_idx,
        &out.inspect_parcel_entity_id);
    if (out.hovered_parcel_idx != (size_t)-1) return out;
    tryPickZone(query, out);
    return out;
}
