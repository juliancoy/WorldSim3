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
        if (fg.rings.empty()) continue;
        if (fg.extent.max_lon < query.view_min_lon || fg.extent.min_lon > query.view_max_lon ||
            fg.extent.max_lat < query.view_min_lat || fg.extent.min_lat > query.view_max_lat) {
            continue;
        }
        if (query.mouse_ll.x < fg.extent.min_lon || query.mouse_ll.x > fg.extent.max_lon ||
            query.mouse_ll.y < fg.extent.min_lat || query.mouse_ll.y > fg.extent.max_lat) {
            continue;
        }
        if (!pointInFeature(fg, query.mouse_ll.x, query.mouse_ll.y)) continue;
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
    for (uint32_t zidx : zone_candidates) {
        if (zidx >= zfeats.size()) continue;
        const auto& zf = zfeats[zidx];
        if (zf.rings.empty()) continue;
        if (query.mouse_ll.x < zf.extent.min_lon || query.mouse_ll.x > zf.extent.max_lon ||
            query.mouse_ll.y < zf.extent.min_lat || query.mouse_ll.y > zf.extent.max_lat) {
            continue;
        }
        if (!pointInFeature(zf, query.mouse_ll.x, query.mouse_ll.y)) continue;
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

        const ImVec2 point_world = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, query.math_zoom);
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
