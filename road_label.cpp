#include "road_label.h"

#include "app_utils.h"
#include "feature_props.h"

#include <algorithm>

namespace {
std::string firstRoadLabelProperty(const FeaturePropertyPairs& values) {
    constexpr const char* keys[] = {
        "FULLNAME", "FullName", "FULL_NAME", "FULL_STREET_NAME", "STREET_FULL_NAME",
        "STREETNAME", "StreetName", "STREET_NAME", "ST_NAME", "STNAME",
        "ROADNAMESHA", "ROAD_NAME_SHA",
        "ROAD_NAME", "RD_NAME", "ROUTE_NAME", "HIGHWAY_NAME", "HWY_NAME",
        "NAME", "Name", "LABEL", "Label", "ROUTE", "Route", "RTE", "HWY"
    };
    for (const char* key : keys) {
        for (const auto& kv : values) {
            if (kv.first == key) {
                std::string label = trimDisplayValue(kv.second);
                if (!label.empty()) return label;
            }
        }
    }
    return {};
}
}

bool isRoadLabelLayer(const LayerDef& layer) {
    if (!layerUsesPolylineGeometry(layer)) return false;
    const std::string haystack = layer.name + " " + layer.logical_id + " " + layer.file + " " + layer.subcategory + " " + layer.description;
    if (containsCaseInsensitive(haystack, "bus") ||
        containsCaseInsensitive(haystack, "transit") ||
        containsCaseInsensitive(haystack, "rail") ||
        containsCaseInsensitive(haystack, "bike") ||
        containsCaseInsensitive(haystack, "bicycle") ||
        containsCaseInsensitive(haystack, "trail")) {
        return false;
    }
    return containsCaseInsensitive(haystack, "street") ||
           containsCaseInsensitive(haystack, "road") ||
           containsCaseInsensitive(haystack, "highway") ||
           containsCaseInsensitive(haystack, "route") ||
           containsCaseInsensitive(haystack, "centerline");
}

std::string roadLabelForFeatureProperties(const LayerDef::FeatureProperties& properties) {
    std::string label = firstRoadLabelProperty(properties.values);
    if (label.empty()) label = "Unnamed road";
    return label;
}

std::string roadLabelForFeature(const LayerDef& layer, size_t feature_idx) {
    std::string label;
    if (feature_idx < layer.feature_properties.size()) {
        label = firstRoadLabelProperty(layer.feature_properties[feature_idx].values);
    }
    if (label.empty()) {
        label = firstDisplayProperty(layer, feature_idx, {
            "FULLNAME", "FullName", "FULL_NAME", "FULL_STREET_NAME", "STREET_FULL_NAME",
            "STREETNAME", "StreetName", "STREET_NAME", "ST_NAME", "STNAME",
            "ROADNAMESHA", "ROAD_NAME_SHA",
            "ROAD_NAME", "RD_NAME", "ROUTE_NAME", "HIGHWAY_NAME", "HWY_NAME",
            "NAME", "Name", "LABEL", "Label", "ROUTE", "Route", "RTE", "HWY"
        });
    }
    if (label.empty()) label = "Unnamed road";
    return label;
}

float squaredDistancePointToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b, ImVec2* closest) {
    const float vx = b.x - a.x;
    const float vy = b.y - a.y;
    const float wx = p.x - a.x;
    const float wy = p.y - a.y;
    const float len_sq = vx * vx + vy * vy;
    float t = len_sq > 0.0f ? (wx * vx + wy * vy) / len_sq : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const ImVec2 c(a.x + vx * t, a.y + vy * t);
    if (closest) *closest = c;
    const float dx = p.x - c.x;
    const float dy = p.y - c.y;
    return dx * dx + dy * dy;
}
