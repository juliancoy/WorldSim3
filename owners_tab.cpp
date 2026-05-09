#include "owners_tab.h"

#include "app_utils.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace {
std::string ownerClassLabel(const std::vector<std::pair<std::string, std::string>>& items, const std::string& key) {
    for (const auto& kv : items) {
        if (kv.first == key) return kv.second;
    }
    return "Unknown";
}
}

void drawOwnersTab(const OwnersTabContext& ctx) {
    if (!ImGui::BeginTabItem("Owners")) return;

    if (!ctx.owner_aggregates || !ctx.selected_owners || !ctx.owner_class_overrides ||
        !ctx.owner_info_state || !ctx.owner_sort_mode || !ctx.owner_sorted_mode ||
        !ctx.owner_class_filter_mode || !ctx.owner_class_assign_mode ||
        !ctx.selected_owner_anchor || !ctx.owner_class_overrides_dirty ||
        !ctx.owner_aggregates_dirty || !ctx.owner_search_query ||
        !ctx.owner_class_items) {
        ImGui::TextDisabled("Owner tab context is incomplete.");
        ImGui::EndTabItem();
        return;
    }

    auto& owner_aggregates = *ctx.owner_aggregates;
    auto& selected_owners = *ctx.selected_owners;
    const auto& owner_class_items = *ctx.owner_class_items;

    ImGui::TextUnformatted("Owner Rankings");
    ImGui::Separator();
    const char* sort_items[] = {"# Properties", "Area Owned", "Value Owned"};
    ImGui::Combo("Sort By", ctx.owner_sort_mode, sort_items, IM_ARRAYSIZE(sort_items));
    if (*ctx.owner_sorted_mode != *ctx.owner_sort_mode) {
        std::stable_sort(owner_aggregates.begin(), owner_aggregates.end(), [&](const OwnerAggregate& a, const OwnerAggregate& b) {
            auto tie_break = [&]() {
                if (a.property_count != b.property_count) return a.property_count > b.property_count;
                if (std::abs(a.area_m2 - b.area_m2) > 0.5) return a.area_m2 > b.area_m2;
                if (std::abs(a.value_usd - b.value_usd) > 0.5) return a.value_usd > b.value_usd;
                return a.owner < b.owner;
            };
            if (*ctx.owner_sort_mode == 1 && std::abs(a.area_m2 - b.area_m2) > 0.5) return a.area_m2 > b.area_m2;
            if (*ctx.owner_sort_mode == 2 && std::abs(a.value_usd - b.value_usd) > 0.5) return a.value_usd > b.value_usd;
            if (*ctx.owner_sort_mode == 0 && a.property_count != b.property_count) return a.property_count > b.property_count;
            return tie_break();
        });
        *ctx.owner_sorted_mode = *ctx.owner_sort_mode;
    }

    ImGui::Text("Owners: %zu", owner_aggregates.size());
    ImGui::Text("Selected: %zu", selected_owners.size());
    ImGui::SameLine();
    if (ImGui::Button("Clear Selection")) {
        selected_owners.clear();
        *ctx.selected_owner_anchor = -1;
    }

    ImGui::SetNextItemWidth(-100.0f);
    ImGui::InputTextWithHint("##owner_search_query", "Owner Search (text)", ctx.owner_search_query, ctx.owner_search_query_size);
    ImGui::SameLine();
    if (ImGui::Button("Clear Search")) ctx.owner_search_query[0] = '\0';
    const std::string owner_query = trimDisplayValue(ctx.owner_search_query);

    if (*ctx.owner_class_filter_mode < 0 || *ctx.owner_class_filter_mode >= (int)owner_class_items.size()) {
        *ctx.owner_class_filter_mode = 0;
    }
    const std::string owner_class_filter_key = owner_class_items[(size_t)*ctx.owner_class_filter_mode].first;
    std::vector<const char*> owner_class_filter_labels;
    owner_class_filter_labels.reserve(owner_class_items.size());
    for (const auto& kv : owner_class_items) owner_class_filter_labels.push_back(kv.second.c_str());
    ImGui::Combo("Class Filter", ctx.owner_class_filter_mode, owner_class_filter_labels.data(), (int)owner_class_filter_labels.size());

    if (*ctx.owner_class_assign_mode <= 0 || *ctx.owner_class_assign_mode >= (int)owner_class_items.size()) {
        *ctx.owner_class_assign_mode = 1;
    }
    std::vector<const char*> owner_class_assign_labels;
    owner_class_assign_labels.reserve(owner_class_items.size() - 1);
    for (size_t i = 1; i < owner_class_items.size(); ++i) owner_class_assign_labels.push_back(owner_class_items[i].second.c_str());
    int assign_idx = *ctx.owner_class_assign_mode - 1;
    ImGui::Combo("Set Class", &assign_idx, owner_class_assign_labels.data(), (int)owner_class_assign_labels.size());
    *ctx.owner_class_assign_mode = assign_idx + 1;

    ImGui::SameLine();
    if (ImGui::Button("Apply To Selected") && !selected_owners.empty()) {
        const std::string class_key = owner_class_items[(size_t)*ctx.owner_class_assign_mode].first;
        for (const auto& owner : selected_owners) (*ctx.owner_class_overrides)[owner] = class_key;
        *ctx.owner_class_overrides_dirty = true;
        *ctx.owner_aggregates_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Class Override") && !selected_owners.empty()) {
        for (const auto& owner : selected_owners) ctx.owner_class_overrides->erase(owner);
        *ctx.owner_class_overrides_dirty = true;
        *ctx.owner_aggregates_dirty = true;
    }

    if (!selected_owners.empty() && ctx.filtered_aggregate_snapshot && ctx.filtered_aggregate_snapshot->valid) {
        ImGui::TextDisabled("Filtered totals | properties: %zu | area: %.0f m^2 | value: %s",
                            ctx.filtered_aggregate_snapshot->owner_property_count,
                            ctx.filtered_aggregate_snapshot->owner_area_m2,
                            formatUsd(ctx.filtered_aggregate_snapshot->owner_value_usd).c_str());
    }

    if (owner_aggregates.empty()) {
        const size_t parcel_count =
            (ctx.layers && ctx.parcel_layer_idx >= 0 && (size_t)ctx.parcel_layer_idx < ctx.layers->size())
                ? (*ctx.layers)[(size_t)ctx.parcel_layer_idx].features.size()
                : 0;
        const size_t real_property_count =
            (ctx.layers && ctx.real_property_layer_idx >= 0 && (size_t)ctx.real_property_layer_idx < ctx.layers->size())
                ? (*ctx.layers)[(size_t)ctx.real_property_layer_idx].features.size()
                : 0;
        ImGui::TextDisabled("Waiting for owner data: parcels=%zu real_property=%zu", parcel_count, real_property_count);
    }

    ImGui::BeginChild("owner_rankings", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    std::vector<size_t> owner_visible_indices;
    owner_visible_indices.reserve(owner_aggregates.size());
    for (size_t i = 0; i < owner_aggregates.size(); ++i) {
        if (!owner_query.empty() && !containsCaseInsensitive(owner_aggregates[i].owner, owner_query)) continue;
        if (owner_class_filter_key != "all" && owner_aggregates[i].owner_class != owner_class_filter_key) continue;
        owner_visible_indices.push_back(i);
    }

    const size_t visible_rows = owner_visible_indices.size();
    ImGuiListClipper clipper;
    clipper.Begin((int)visible_rows);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const size_t visible_idx = (size_t)row;
            const size_t agg_idx = owner_visible_indices[visible_idx];
            const auto& r = owner_aggregates[agg_idx];
            const bool row_selected = selected_owners.find(r.owner) != selected_owners.end();
            ImGui::PushID((int)agg_idx);
            if (ImGui::Selectable(r.owner.c_str(), row_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                const bool shift = ImGui::GetIO().KeyShift;
                const bool ctrl = ImGui::GetIO().KeyCtrl;
                if (shift && *ctx.selected_owner_anchor >= 0 && (size_t)*ctx.selected_owner_anchor < visible_rows) {
                    if (!ctrl) selected_owners.clear();
                    const size_t begin = std::min((size_t)*ctx.selected_owner_anchor, visible_idx);
                    const size_t end = std::max((size_t)*ctx.selected_owner_anchor, visible_idx);
                    for (size_t j = begin; j <= end; ++j) {
                        selected_owners.insert(owner_aggregates[owner_visible_indices[j]].owner);
                    }
                } else if (ctrl) {
                    if (row_selected) selected_owners.erase(r.owner);
                    else selected_owners.insert(r.owner);
                    *ctx.selected_owner_anchor = (int)visible_idx;
                } else {
                    selected_owners.clear();
                    selected_owners.insert(r.owner);
                    *ctx.selected_owner_anchor = (int)visible_idx;
                }
            }
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::PushID((int)agg_idx + 1000000);
            if (ImGui::SmallButton("Open")) {
                openOwnerInfoPage(*ctx.owner_info_state, r.owner);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Open owner info page");
            }
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", ownerClassLabel(owner_class_items, r.owner_class).c_str());
            ImGui::TextDisabled("properties: %zu | area: %.0f m^2 | value: %s", r.property_count, r.area_m2, formatUsd(r.value_usd).c_str());
            ImGui::Separator();
        }
    }
    if (!owner_query.empty() && visible_rows == 0) {
        ImGui::TextDisabled("No owners match \"%s\".", owner_query.c_str());
    }
    ImGui::EndChild();
    if (!owner_query.empty()) {
        ImGui::TextDisabled("Visible rows: %zu / %zu", visible_rows, owner_aggregates.size());
    } else {
        ImGui::TextDisabled("Showing all owners: %zu", owner_aggregates.size());
    }
    ImGui::EndTabItem();
}
