#include "map_render_hover.h"

#include "feature_props.h"
#include "layer_geometry.h"
#include "zoning.h"

#include <algorithm>
#include <limits>

namespace {
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
}

MapHoverState findMapHoverTargets(const MapHoverQuery& query) {
    MapHoverState out;
    findHoveredParcel(query, out);
    findHoveredZone(query, out);
    return out;
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
