#include "map_render_hover.h"

#include "feature_props.h"
#include "gpu_picking.h"
#include "zoning.h"

#include <cmath>
#include <cstdint>

namespace {
uint64_t mixPointOrderHash(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

uint64_t quantizedCoordHash(float value) {
    const int64_t q = (int64_t)std::llround((double)value * 1000000.0);
    return (uint64_t)q;
}
}

MapHoverState findMapHoverTargets(const MapHoverQuery& query) {
    return findGpuHoverTargets(query);
}

uint64_t stablePointFeatureOrderKey(size_t layer_idx, size_t feature_idx, const LayerDef::FeatureRecord& fg) {
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
    const LayerDef::FeatureRecord& zone,
    const std::unordered_map<std::string, ZoneMetadata>& zoning_metadata) {
    std::string zone_key = zoningClassKey(zone);
    std::string zone_label = zoningClassLabel(zone);
    auto meta_it = zoning_metadata.find(zone_key);
    if (meta_it != zoning_metadata.end() && !meta_it->second.label.empty()) {
        zone_label = meta_it->second.label;
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
