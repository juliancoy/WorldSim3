#include "zoning_filters_panel.h"

#include "feature_props.h"

#include <algorithm>

bool drawZoningFiltersPanel(ZoningFiltersPanelContext& ctx) {
    if (ctx.zoning_layer_idx < 0 || !ctx.root || !ctx.app_settings ||
        !ctx.zoning_zone_enabled || !ctx.zoning_zone_color || !ctx.zoning_zone_label ||
        !ctx.zoning_metadata || !ctx.zoning_zone_order || !ctx.zoning_zone_counts ||
        !ctx.zoning_group_zones || !ctx.zoning_group_order) {
        return false;
    }
    bool zoning_filters_changed = false;
    std::vector<std::string> fallback_zone_order;
    std::unordered_map<std::string, std::vector<std::string>> fallback_group_zones;
    std::vector<std::string> fallback_group_order;
    const std::vector<std::string>* zone_order = ctx.zoning_zone_order;
    const std::unordered_map<std::string, std::vector<std::string>>* group_zones = ctx.zoning_group_zones;
    const std::vector<std::string>* group_order = ctx.zoning_group_order;
    if (zone_order->empty()) {
        if (ctx.zoning_zone_enabled->empty()) return false;
        fallback_zone_order.reserve(ctx.zoning_zone_enabled->size());
        for (const auto& kv : *ctx.zoning_zone_enabled) fallback_zone_order.push_back(kv.first);
        std::sort(fallback_zone_order.begin(), fallback_zone_order.end());
        for (const std::string& zkey : fallback_zone_order) {
            const std::string gkey = zoningGroupKey(zkey);
            if (fallback_group_zones.find(gkey) == fallback_group_zones.end()) fallback_group_order.push_back(gkey);
            fallback_group_zones[gkey].push_back(zkey);
        }
        std::sort(fallback_group_order.begin(), fallback_group_order.end());
        zone_order = &fallback_zone_order;
        group_zones = &fallback_group_zones;
        group_order = &fallback_group_order;
    }
    if (!ImGui::CollapsingHeader("Zoning Filters", ImGuiTreeNodeFlags_DefaultOpen)) return false;

    if (ImGui::Checkbox("Use SimCity zoning colors##zoning_simcity_colors", &ctx.app_settings->zoning_use_simcity_colors)) {
        for (const auto& zkey : *zone_order) {
            auto meta_it = ctx.zoning_metadata->find(zkey);
            if (ctx.app_settings->zoning_use_simcity_colors) {
                (*ctx.zoning_zone_color)[zkey] = zoningShadeVariant(zoningColorFromConvention(zkey), zkey);
            } else {
                const ImVec4 base_color =
                    (meta_it != ctx.zoning_metadata->end() && meta_it->second.has_color) ? meta_it->second.color : colorFromStableKey(zkey);
                (*ctx.zoning_zone_color)[zkey] = zoningShadeVariant(base_color, zkey);
            }
        }
        saveAppSettings(*ctx.root, *ctx.app_settings);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("ON: SimCity-style palette (R=green, C=blue, I=yellow). OFF: zoning metadata/default colors.");
        ImGui::EndTooltip();
    }
    if (ImGui::Button("Show All Zones")) {
        for (const auto& zkey : *zone_order) {
            bool& enabled = (*ctx.zoning_zone_enabled)[zkey];
            if (!enabled) zoning_filters_changed = true;
            enabled = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Hide All Zones")) {
        for (const auto& zkey : *zone_order) {
            bool& enabled = (*ctx.zoning_zone_enabled)[zkey];
            if (enabled) zoning_filters_changed = true;
            enabled = false;
        }
    }
    for (const auto& gkey : *group_order) {
        auto git = group_zones->find(gkey);
        if (git == group_zones->end()) continue;
        const auto& zones = git->second;
        size_t enabled_count = 0;
        for (const auto& z : zones) if ((*ctx.zoning_zone_enabled)[z]) enabled_count++;
        bool group_enabled = enabled_count == zones.size() && !zones.empty();
        bool group_partial = enabled_count > 0 && enabled_count < zones.size();

        if (group_partial) ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.0f, 0.75f, 0.1f, 1.0f));
        std::string gcb = gkey + "##group_enabled";
        bool group_next = group_enabled;
        if (ImGui::Checkbox(gcb.c_str(), &group_next)) {
            for (const auto& z : zones) {
                if ((*ctx.zoning_zone_enabled)[z] != group_next) zoning_filters_changed = true;
                (*ctx.zoning_zone_enabled)[z] = group_next;
            }
        }
        if (group_partial) ImGui::PopStyleColor();
        ImGui::SameLine();
        std::string gheader = gkey + " Zones (" + std::to_string(enabled_count) + "/" + std::to_string(zones.size()) + ")";
        if (ImGui::TreeNodeEx(gheader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& zkey : zones) {
                auto color_it = ctx.zoning_zone_color->find(zkey);
                if (color_it == ctx.zoning_zone_color->end()) {
                    color_it = ctx.zoning_zone_color->emplace(
                        zkey,
                        zoningShadeVariant(zoningColorFromConvention(zkey), zkey)).first;
                }
                ImVec4 zc = color_it->second;
                ImGui::ColorButton((std::string("##zclr_") + zkey).c_str(), zc, ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
                ImGui::SameLine();
                bool enabled = (*ctx.zoning_zone_enabled)[zkey];
                std::string display = zkey;
                auto lit = ctx.zoning_zone_label->find(zkey);
                if (lit != ctx.zoning_zone_label->end() && !lit->second.empty() && lit->second != zkey) {
                    display += " - " + lit->second;
                }
                const auto count_it = ctx.zoning_zone_counts->find(zkey);
                const size_t count = count_it == ctx.zoning_zone_counts->end() ? 0 : count_it->second;
                std::string label = display + " (" + std::to_string(count) + ")";
                if (ImGui::Checkbox(label.c_str(), &enabled)) {
                    (*ctx.zoning_zone_enabled)[zkey] = enabled;
                    zoning_filters_changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(440.0f);
                    ImGui::TextUnformatted(zkey.c_str());
                    auto mit = ctx.zoning_metadata->find(zkey);
                    if (mit != ctx.zoning_metadata->end()) {
                        if (!mit->second.label.empty()) ImGui::TextWrapped("%s", mit->second.label.c_str());
                        if (!mit->second.description.empty()) {
                            ImGui::Separator();
                            ImGui::TextWrapped("%s", mit->second.description.c_str());
                        }
                    }
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }
            ImGui::TreePop();
        }
    }
    return zoning_filters_changed;
}
