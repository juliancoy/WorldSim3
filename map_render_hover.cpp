#include "map_render_hover.h"

#include "app_utils.h"
#include "feature_props.h"
#include "geo.h"
#include "layer_geometry.h"
#include "zoning.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
constexpr float kPointHoverRadiusPx = 10.0f;
constexpr int kPointClusterMaxMathZoom = 15;

uint64_t mixPointOrderHash(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

uint64_t quantizedCoordHash(float value) {
    const int64_t q = (int64_t)std::llround((double)value * 1000000.0);
    return (uint64_t)q;
}

const PolygonGeometryArtifact* polygonArtifactForLayer(const MapHoverQuery& query, size_t layer_idx) {
    if (!query.polygon_geometry_artifacts) return nullptr;
    auto it = query.polygon_geometry_artifacts->find(layer_idx);
    if (it == query.polygon_geometry_artifacts->end()) return nullptr;
    return &it->second;
}

const ParcelRenderFeatureRecord* parcelRenderFeature(const MapHoverQuery& query, size_t parcel_idx) {
    if (!query.parcel_render_blob) return nullptr;
    if (parcel_idx < query.parcel_render_blob->features.size() &&
        query.parcel_render_blob->features[parcel_idx].feature_idx == parcel_idx) {
        return &query.parcel_render_blob->features[parcel_idx];
    }
    for (const ParcelRenderFeatureRecord& rec : query.parcel_render_blob->features) {
        if (rec.feature_idx == parcel_idx) return &rec;
    }
    return nullptr;
}

bool pointInTriangleLonLat(
    float px, float py,
    const ImVec2& a,
    const ImVec2& b,
    const ImVec2& c) {
    auto cross = [](const ImVec2& u, const ImVec2& v, float x, float y) {
        return (v.x - u.x) * (y - u.y) - (v.y - u.y) * (x - u.x);
    };
    const float c1 = cross(a, b, px, py);
    const float c2 = cross(b, c, px, py);
    const float c3 = cross(c, a, px, py);
    const bool has_neg = (c1 < 0.0f) || (c2 < 0.0f) || (c3 < 0.0f);
    const bool has_pos = (c1 > 0.0f) || (c2 > 0.0f) || (c3 > 0.0f);
    return !(has_neg && has_pos);
}

bool pointInPolygonArtifactFeature(
    const PolygonGeometryArtifact& artifact,
    size_t feature_idx,
    float lon,
    float lat) {
    if (feature_idx >= artifact.features.size()) return false;
    const GeometryArtifactFeatureRecord& rec = artifact.features[feature_idx];
    const uint32_t end = rec.index_offset + rec.index_count;
    if (end > artifact.fill_indices.size()) return false;
    for (uint32_t i = rec.index_offset; i + 2 < end; i += 3) {
        const uint32_t ia = artifact.fill_indices[i];
        const uint32_t ib = artifact.fill_indices[i + 1];
        const uint32_t ic = artifact.fill_indices[i + 2];
        if (ia >= artifact.vertices.size() || ib >= artifact.vertices.size() || ic >= artifact.vertices.size()) continue;
        if (pointInTriangleLonLat(lon, lat, artifact.vertices[ia], artifact.vertices[ib], artifact.vertices[ic])) {
            return true;
        }
    }
    return false;
}

bool pointInParcelRenderFeature(
    const MapHoverQuery& query,
    const ParcelRenderFeatureRecord& rec,
    float lon,
    float lat) {
    if (!query.parcel_render_blob) return false;
    const uint32_t end = rec.index_offset + rec.index_count;
    if (end > query.parcel_render_blob->indices.size()) return false;
    for (uint32_t i = rec.index_offset; i + 2 < end; i += 3) {
        const uint32_t ia = query.parcel_render_blob->indices[i];
        const uint32_t ib = query.parcel_render_blob->indices[i + 1];
        const uint32_t ic = query.parcel_render_blob->indices[i + 2];
        if (ia >= query.parcel_render_blob->vertices.size() ||
            ib >= query.parcel_render_blob->vertices.size() ||
            ic >= query.parcel_render_blob->vertices.size()) continue;
        if (pointInTriangleLonLat(
                lon,
                lat,
                query.parcel_render_blob->vertices[ia],
                query.parcel_render_blob->vertices[ib],
                query.parcel_render_blob->vertices[ic])) {
            return true;
        }
    }
    return false;
}

ImVec2 pointWorldPosition(const MapHoverQuery& query, size_t layer_idx, size_t feature_idx, const LayerDef::FeatureGeom& fg) {
    if (query.point_geometry_artifacts) {
        auto it = query.point_geometry_artifacts->find(layer_idx);
        if (it != query.point_geometry_artifacts->end()) {
            const PointGeometryArtifact& artifact = it->second;
            if (feature_idx < artifact.positions.size()) {
                return lonLatToWorldPx(artifact.positions[feature_idx].x, artifact.positions[feature_idx].y, query.math_zoom);
            }
        }
    }
    return lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, query.math_zoom);
}

void findHoveredParcel(const MapHoverQuery& query, MapHoverState& out) {
    if (!query.map_hovered || (!query.parcel_hover_active && !query.parcel_inspect_active) ||
        query.parcel_layer_idx < 0 || !query.layers || !query.layer_spatial) {
        return;
    }
    const size_t pli = (size_t)query.parcel_layer_idx;
    if (pli >= query.layers->size() || pli >= query.layer_spatial->size() || !(*query.layer_spatial)[pli].built) return;

    std::vector<uint32_t> hover_candidates;
    if (!queryLayerSpatialIndex(
            (*query.layer_spatial)[pli],
            query.mouse_ll.x,
            query.mouse_ll.y,
            query.mouse_ll.x,
            query.mouse_ll.y,
            hover_candidates)) {
        return;
    }

    float best_area = std::numeric_limits<float>::infinity();
    const auto& features = (*query.layers)[pli].features;
    for (uint32_t fidx : hover_candidates) {
        if (fidx >= features.size()) continue;
        const auto& fg = features[(size_t)fidx];
        const ParcelRenderFeatureRecord* parcel_feature = parcelRenderFeature(query, (size_t)fidx);
        if (!parcel_feature) continue;
        if (fg.extent.max_lon < query.view_min_lon || fg.extent.min_lon > query.view_max_lon ||
            fg.extent.max_lat < query.view_min_lat || fg.extent.min_lat > query.view_max_lat) {
            continue;
        }
        if (query.mouse_ll.x < fg.extent.min_lon || query.mouse_ll.x > fg.extent.max_lon ||
            query.mouse_ll.y < fg.extent.min_lat || query.mouse_ll.y > fg.extent.max_lat) {
            continue;
        }
        const bool contains_point =
            pointInParcelRenderFeature(query, *parcel_feature, query.mouse_ll.x, query.mouse_ll.y);
        if (!contains_point) continue;
        const float area = std::max(0.0f, fg.extent.max_lon - fg.extent.min_lon) *
                           std::max(0.0f, fg.extent.max_lat - fg.extent.min_lat);
        if (area < best_area) {
            best_area = area;
            out.hovered_parcel = &fg;
            out.hovered_parcel_idx = (size_t)fidx;
        }
    }
}

void findHoveredZone(const MapHoverQuery& query, MapHoverState& out) {
    if (!query.map_hovered || (!query.zoning_hover_active && !query.zoning_inspect_active) ||
        query.zoning_layer_idx < 0 || !query.layers || !query.layer_spatial) {
        return;
    }
    const size_t zli = (size_t)query.zoning_layer_idx;
    if (zli >= query.layers->size() || zli >= query.layer_spatial->size() || !(*query.layer_spatial)[zli].built) return;

    std::vector<uint32_t> zone_candidates;
    if (!queryLayerSpatialIndex(
            (*query.layer_spatial)[zli],
            query.mouse_ll.x,
            query.mouse_ll.y,
            query.mouse_ll.x,
            query.mouse_ll.y,
            zone_candidates)) {
        return;
    }

    float best_area = std::numeric_limits<float>::infinity();
    const auto& zfeats = (*query.layers)[zli].features;
    const PolygonGeometryArtifact* polygon_artifact = polygonArtifactForLayer(query, zli);
    for (uint32_t zidx : zone_candidates) {
        if (zidx >= zfeats.size()) continue;
        const auto& zf = zfeats[zidx];
        const bool has_artifact_geometry = polygon_artifact && (size_t)zidx < polygon_artifact->features.size();
        if (!has_artifact_geometry) continue;
        if (query.mouse_ll.x < zf.extent.min_lon || query.mouse_ll.x > zf.extent.max_lon ||
            query.mouse_ll.y < zf.extent.min_lat || query.mouse_ll.y > zf.extent.max_lat) {
            continue;
        }
        const bool contains_point =
            pointInPolygonArtifactFeature(*polygon_artifact, (size_t)zidx, query.mouse_ll.x, query.mouse_ll.y);
        if (!contains_point) continue;
        const float area = std::max(0.0f, zf.extent.max_lon - zf.extent.min_lon) *
                           std::max(0.0f, zf.extent.max_lat - zf.extent.min_lat);
        if (area < best_area) {
            best_area = area;
            out.hovered_zone = &zf;
            out.hovered_zone_idx = (size_t)zidx;
        }
    }
}

void findHoveredPointFeature(const MapHoverQuery& query, MapHoverState& out) {
    if (!query.map_hovered || !query.point_hover_active || !query.layers || !query.layer_spatial || !query.layer_hover_enabled ||
        !query.project_world ||
        query.viewport_size.x <= 1.0f || query.viewport_size.y <= 1.0f) {
        return;
    }

    const float lon_pad = std::max(
        0.0001f,
        (query.view_max_lon - query.view_min_lon) * ((kPointHoverRadiusPx + 2.0f) / query.viewport_size.x));
    const float lat_pad = std::max(
        0.0001f,
        (query.view_max_lat - query.view_min_lat) * ((kPointHoverRadiusPx + 2.0f) / query.viewport_size.y));
    float best_dist_sq = kPointHoverRadiusPx * kPointHoverRadiusPx;
    uint64_t best_order_key = 0;

    if (query.active_hover_layer_idx < 0) return;
    const size_t layer_idx = (size_t)query.active_hover_layer_idx;
    if (layer_idx >= query.layers->size() || layer_idx >= query.layer_spatial->size() || layer_idx >= query.layer_hover_enabled->size()) {
        return;
    }
    if (!(*query.layer_hover_enabled)[layer_idx]) return;

    const LayerDef& layer = (*query.layers)[layer_idx];
    if (!layer.enabled || !layerUsesPointGeometry(layer) || !(*query.layer_spatial)[layer_idx].built) return;

    std::vector<uint32_t> point_candidates;
    if (!queryLayerSpatialIndex(
            (*query.layer_spatial)[layer_idx],
            query.mouse_ll.x - lon_pad,
            query.mouse_ll.y - lat_pad,
            query.mouse_ll.x + lon_pad,
            query.mouse_ll.y + lat_pad,
            point_candidates)) {
        return;
    }

    const auto& features = layer.features;
    for (uint32_t fidx : point_candidates) {
        if (fidx >= features.size()) continue;
        const auto& fg = features[(size_t)fidx];
        if (fg.extent.max_lon < query.view_min_lon || fg.extent.min_lon > query.view_max_lon ||
            fg.extent.max_lat < query.view_min_lat || fg.extent.min_lat > query.view_max_lat) {
            continue;
        }

        const ImVec2 point_world = pointWorldPosition(query, layer_idx, (size_t)fidx, fg);
        const ImVec2 point_screen = query.project_world(point_world);
        const float dx = point_screen.x - query.mouse_screen.x;
        const float dy = point_screen.y - query.mouse_screen.y;
        const float dist_sq = dx * dx + dy * dy;
        if (dist_sq > best_dist_sq) continue;
        const uint64_t order_key = stablePointFeatureOrderKey(layer_idx, (size_t)fidx, fg);
        const bool better_distance = dist_sq + 0.01f < best_dist_sq;
        const bool equal_distance = std::fabs(dist_sq - best_dist_sq) <= 0.01f;
        if (!better_distance && !(equal_distance && order_key >= best_order_key)) continue;

        best_dist_sq = dist_sq;
        best_order_key = order_key;
        out.hovered_point = &fg;
        out.hovered_point_idx = (size_t)fidx;
        out.hovered_point_layer_idx = (int)layer_idx;
    }
}
}

MapHoverState findMapHoverTargets(const MapHoverQuery& query) {
    MapHoverState out;
    findHoveredPointFeature(query, out);
    if (out.hovered_point != nullptr) return out;

    findHoveredParcel(query, out);
    if (out.hovered_parcel != nullptr) return out;

    findHoveredZone(query, out);
    return out;
}

uint64_t stablePointFeatureOrderKey(size_t layer_idx, size_t feature_idx, const LayerDef::FeatureGeom& fg) {
    uint64_t h = 1469598103934665603ULL;
    h = mixPointOrderHash(h, (uint64_t)layer_idx);
    h = mixPointOrderHash(h, (uint64_t)feature_idx);
    h = mixPointOrderHash(h, quantizedCoordHash(fg.extent.min_lon));
    h = mixPointOrderHash(h, quantizedCoordHash(fg.extent.min_lat));
    h = mixPointOrderHash(h, quantizedCoordHash(fg.extent.max_lon));
    h = mixPointOrderHash(h, quantizedCoordHash(fg.extent.max_lat));
    return h;
}

void drawZoningHoverTooltip(
    const LayerDef::FeatureGeom& zone,
    const std::unordered_map<std::string, ZoneMetadata>& zoning_metadata) {
    std::string zone_key = zoningClassKey(zone);
    std::string zone_label = zoningClassLabel(zone);
    auto meta_it = zoning_metadata.find(zone_key);
    if (meta_it != zoning_metadata.end()) {
        if (!meta_it->second.label.empty()) zone_label = meta_it->second.label;
    }
    std::string zone_description = zoningDescription(zone, zoning_metadata);
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(440.0f);
    const char* display_zone = !zone_label.empty() ? zone_label.c_str() : (zone_key.empty() ? "(unlabeled)" : zone_key.c_str());
    ImGui::SetWindowFontScale(1.45f);
    ImGui::TextWrapped("%s", display_zone);
    ImGui::SetWindowFontScale(1.0f);
    if (!zone_description.empty()) ImGui::TextWrapped("%s", zone_description.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
