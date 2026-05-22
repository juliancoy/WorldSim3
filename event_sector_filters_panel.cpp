#include "event_sector_filters_panel.h"

#include "event_sectors.h"
#include "imgui.h"

#include <unordered_map>

bool drawEventSectorFiltersPanel(EventSectorFiltersPanelContext& ctx) {
    if (!ctx.layers || !ctx.map_filter_state) return false;

    std::unordered_map<std::string, size_t> counts;
    size_t total_event_features = 0;
    for (const auto& def : communitySectorDefs()) counts[def.label] = 0;
    for (const LayerDef& layer : *ctx.layers) {
        if (!layer.enabled || !isCommunitySectorEventLayer(layer)) continue;
        for (const auto& fg : layer.features) {
            counts[classifyCommunitySector(fg)]++;
            total_event_features++;
        }
    }
    if (total_event_features == 0) return false;
    if (!ImGui::CollapsingHeader("Community Sectors", ImGuiTreeNodeFlags_DefaultOpen)) return false;

    bool changed = false;
    auto& enabled = const_cast<MapFilterState*>(ctx.map_filter_state)->event_sector_enabled;
    if (ImGui::Button("Show All Sectors")) {
        for (const auto& def : communitySectorDefs()) {
            if (!enabled[def.label]) changed = true;
            enabled[def.label] = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Hide All Sectors")) {
        for (const auto& def : communitySectorDefs()) {
            if (enabled[def.label]) changed = true;
            enabled[def.label] = false;
        }
    }
    for (const auto& def : communitySectorDefs()) {
        ImGui::ColorButton((std::string("##sector_") + def.label).c_str(), def.color, ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
        ImGui::SameLine();
        bool is_enabled = enabled[def.label];
        const std::string label = def.label + " (" + std::to_string(counts[def.label]) + ")";
        if (ImGui::Checkbox(label.c_str(), &is_enabled)) {
            enabled[def.label] = is_enabled;
            changed = true;
        }
        if (ImGui::IsItemHovered() && !def.matches.empty()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(def.label.c_str());
            ImGui::Separator();
            for (const std::string& match : def.matches) ImGui::TextWrapped("%s", match.c_str());
            ImGui::EndTooltip();
        }
    }
    return changed;
}
