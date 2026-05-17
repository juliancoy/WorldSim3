#include "active_queries_tab.h"

#include "app_utils.h"
#include "feature_props.h"
#include "imgui.h"

#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

namespace {
void drawResultSetSummary(const char* label, const FilterResultSet& result_set) {
    ImGui::Text("%s", label);
    ImGui::BulletText(
        "active=%s, layers=%zu, features=%zu, blocklots=%zu, owners=%zu",
        result_set.active ? "true" : "false",
        result_set.layers.size(),
        result_set.features.size(),
        result_set.blocklots.size(),
        result_set.owners.size());
}

void drawOptionalFilterField(const char* label, const char* value) {
    if (!value || value[0] == '\0') return;
    ImGui::BulletText("%s: %s", label, value);
}

void drawEnabledLayersSummary(const std::vector<LayerDef>& layers) {
    size_t enabled_count = 0;
    for (const auto& layer : layers) {
        if (layer.enabled) ++enabled_count;
    }

    ImGui::SeparatorText("Visible Layers");
    ImGui::Text("Enabled layers: %zu / %zu", enabled_count, layers.size());
    if (enabled_count == 0) {
        ImGui::TextDisabled("No visible layers.");
        return;
    }

    if (ImGui::BeginChild("active_queries_visible_layers", ImVec2(0, 120), true)) {
        for (size_t i = 0; i < layers.size(); ++i) {
            const auto& layer = layers[i];
            if (!layer.enabled) continue;
            ImGui::BulletText(
                "%s [%s | %s | %zu features]",
                layer.name.c_str(),
                categoryToString(layer.category),
                layer.scale.empty() ? "unspecified scale" : layer.scale.c_str(),
                layer.features.size());
        }
    }
    ImGui::EndChild();
}

std::string joinPreviewList(const std::vector<std::string>& values, size_t max_items) {
    std::ostringstream os;
    for (size_t i = 0; i < values.size() && i < max_items; ++i) {
        if (i > 0) os << ", ";
        os << values[i];
    }
    if (values.size() > max_items) os << ", ...";
    return os.str();
}

void drawPredicateStack(const ActiveQueriesTabContext& ctx) {
    const MapFilterState& filters = *ctx.map_filter_state;
    ImGui::SeparatorText("Visibility Predicate Stack");

    const bool selected_owner_gate_active = !filters.selected_owners.empty();
    const bool field_gate_active =
        filters.enabled &&
        (filters.use_date ||
         filters.blocklot[0] != '\0' ||
         filters.status[0] != '\0' ||
         filters.address[0] != '\0' ||
         filters.owner[0] != '\0' ||
         filters.zip[0] != '\0');
    const bool crime_gate_active = filters.crime.enabled;
    const bool result_set_gate_active = ctx.active_result_set->active;

    ImGui::BulletText("Layer visibility toggle: %zu enabled layers participate in draw planning.", [&]() {
        size_t enabled_count = 0;
        for (const auto& layer : *ctx.layers) if (layer.enabled) ++enabled_count;
        return enabled_count;
    }());

    ImGui::BulletText(
        "Renderer result-set gate: %s",
        result_set_gate_active ? "active" : "inactive");
    if (result_set_gate_active) {
        ImGui::TextWrapped(
            "Applies before standard field filters. Matching identities come from feature keys, blocklots, or owners.");
    }

    ImGui::BulletText(
        "Selected-owner gate: %s",
        selected_owner_gate_active ? "active on parcel-related layers" : "inactive");
    if (selected_owner_gate_active) {
        ImGui::Text("Selected owners: %zu", filters.selected_owners.size());
    }

    ImGui::BulletText(
        "General UI filter gate: %s",
        field_gate_active ? "active" : (filters.enabled ? "enabled but unconstrained" : "inactive"));
    if (field_gate_active) {
        std::vector<std::string> clauses;
        if (filters.use_date) clauses.push_back("record year");
        if (filters.blocklot[0] != '\0') clauses.push_back("block/lot");
        if (filters.status[0] != '\0') clauses.push_back("status");
        if (filters.address[0] != '\0') clauses.push_back("address");
        if (filters.owner[0] != '\0') clauses.push_back("owner");
        if (filters.zip[0] != '\0') clauses.push_back("zip");
        ImGui::TextWrapped("Active fields: %s", joinPreviewList(clauses, 8).c_str());
    }

    ImGui::BulletText(
        "Crime filter gate: %s",
        crime_gate_active ? "active on the crime layer only" : "inactive");
    if (crime_gate_active) {
        std::vector<std::string> crime_types;
        if (filters.crime.homicide) crime_types.push_back("homicide");
        if (filters.crime.robbery) crime_types.push_back("robbery");
        if (filters.crime.assault) crime_types.push_back("assault");
        if (filters.crime.burglary) crime_types.push_back("burglary");
        if (filters.crime.theft) crime_types.push_back("theft");
        if (filters.crime.auto_theft) crime_types.push_back("auto theft");
        if (filters.crime.drug) crime_types.push_back("drug");
        if (filters.crime.shooting) crime_types.push_back("shooting");
        if (!crime_types.empty()) {
            ImGui::TextWrapped("Crime categories: %s", joinPreviewList(crime_types, 8).c_str());
        } else {
            ImGui::TextDisabled("No crime categories selected; only the optional year range applies.");
        }
    }
}

void drawZoningPredicateSummary(const ActiveQueriesTabContext& ctx) {
    ImGui::SeparatorText("Zoning Predicate");
    if (ctx.zoning_layer_idx < 0) {
        ImGui::TextDisabled("No zoning layer is configured.");
        return;
    }
    if (!ctx.zoning_zone_enabled) {
        ImGui::TextDisabled("Zoning predicate state is unavailable.");
        return;
    }

    size_t enabled_count = 0;
    std::vector<std::string> hidden_zones;
    hidden_zones.reserve(ctx.zoning_zone_enabled->size());
    for (const auto& kv : *ctx.zoning_zone_enabled) {
        if (kv.second) {
            ++enabled_count;
        } else {
            hidden_zones.push_back(kv.first);
        }
    }
    ImGui::Text("Enabled zoning classes: %zu / %zu", enabled_count, ctx.zoning_zone_enabled->size());
    if (hidden_zones.empty()) {
        ImGui::TextDisabled("No zoning classes are hidden.");
    } else {
        ImGui::TextWrapped("Hidden zoning classes: %s", joinPreviewList(hidden_zones, 12).c_str());
    }
}

void drawRenderGateSummary(const ActiveQueriesTabContext& ctx) {
    ImGui::SeparatorText("Render Gates");
    if (!ctx.layer_fill_enabled || !ctx.layers) {
        ImGui::TextDisabled("Per-layer render gate state is unavailable.");
        return;
    }

    std::vector<std::string> fill_disabled_layers;
    for (size_t i = 0; i < ctx.layers->size() && i < ctx.layer_fill_enabled->size(); ++i) {
        const auto& layer = (*ctx.layers)[i];
        if (!layer.enabled) continue;
        if (layer.features.empty()) continue;
        if ((*ctx.layer_fill_enabled)[i]) continue;
        fill_disabled_layers.push_back(layer.name);
    }

    if (fill_disabled_layers.empty()) {
        ImGui::TextDisabled("No enabled layers currently have fill rendering disabled.");
    } else {
        ImGui::TextWrapped(
            "Polygon fill disabled for enabled layers: %s",
            joinPreviewList(fill_disabled_layers, 12).c_str());
        ImGui::TextDisabled("This affects polygon fill visibility, not necessarily outlines or point markers.");
    }
}

bool isParcelRelatedLayerForUi(const LayerDef& layer) {
    return layer.scale == "parcel" && layer.category != LayerDef::Category::Zoning;
}

bool layerTargetedByActiveResultSet(const ActiveQueriesTabContext& ctx, size_t layer_idx) {
    if (!ctx.active_result_set || !ctx.active_result_set->active) return false;
    if (ctx.active_result_set->layers.empty()) return true;
    if (ctx.active_result_set->layers.find(layer_idx) != ctx.active_result_set->layers.end()) return true;
    if (layer_idx < ctx.layers->size() && isParcelRelatedLayerForUi((*ctx.layers)[layer_idx])) return true;
    return false;
}

size_t hiddenZoningClassCountForLayer(const ActiveQueriesTabContext& ctx, size_t layer_idx) {
    if (!ctx.zoning_zone_enabled || (int)layer_idx != ctx.zoning_layer_idx || layer_idx >= ctx.layers->size()) return 0;
    size_t hidden = 0;
    for (const auto& fg : (*ctx.layers)[layer_idx].features) {
        const std::string zkey = zoningClassKey(fg);
        auto it = ctx.zoning_zone_enabled->find(zkey);
        if (it != ctx.zoning_zone_enabled->end() && !it->second) ++hidden;
    }
    return hidden;
}

void addReason(std::vector<std::string>& reasons, const std::string& reason) {
    reasons.push_back(reason);
}

void drawPerLayerReasonSummary(const ActiveQueriesTabContext& ctx) {
    if (!ctx.layers) return;
    ImGui::SeparatorText("Per-Layer Visibility Reasons");
    if (ImGui::BeginChild("active_queries_layer_reasons", ImVec2(0, 220), true)) {
        for (size_t i = 0; i < ctx.layers->size(); ++i) {
            const LayerDef& layer = (*ctx.layers)[i];
            std::vector<std::string> reasons;
            std::vector<std::string> gates;

            if (!layer.enabled) {
                addReason(reasons, "hidden by main layer visibility toggle");
            } else {
                addReason(reasons, "visible in draw plan");
            }

            if (ctx.active_result_set && ctx.active_result_set->active && layerTargetedByActiveResultSet(ctx, i)) {
                addReason(gates, "active result-set gate");
            }

            const bool parcel_related = isParcelRelatedLayerForUi(layer);
            if (parcel_related && ctx.map_filter_state && !ctx.map_filter_state->selected_owners.empty()) {
                addReason(gates, "selected-owner gate");
            }

            if (ctx.map_filter_state && ctx.map_filter_state->enabled) {
                const bool has_field_filter =
                    ctx.map_filter_state->use_date ||
                    ctx.map_filter_state->blocklot[0] != '\0' ||
                    ctx.map_filter_state->status[0] != '\0' ||
                    ctx.map_filter_state->address[0] != '\0' ||
                    ctx.map_filter_state->owner[0] != '\0' ||
                    ctx.map_filter_state->zip[0] != '\0';
                if (has_field_filter) addReason(gates, "general field/date gate");
            }

            if ((int)i == ctx.crime_nibrs_layer_idx && ctx.map_filter_state && ctx.map_filter_state->crime.enabled) {
                addReason(gates, "crime-only gate");
            }

            const size_t hidden_zones = hiddenZoningClassCountForLayer(ctx, i);
            if (hidden_zones > 0) {
                addReason(reasons, std::to_string(hidden_zones) + " zoning features hidden by class toggle");
            }

            if (ctx.layer_fill_enabled && i < ctx.layer_fill_enabled->size() && !(*ctx.layer_fill_enabled)[i] && !layer.features.empty()) {
                addReason(reasons, "polygon fill rendering disabled");
            }

            ImGui::PushID((int)i);
            if (ImGui::TreeNode(layer.name.c_str())) {
                ImGui::TextDisabled("%s | %s | %zu features",
                    categoryToString(layer.category),
                    layer.scale.empty() ? "unspecified scale" : layer.scale.c_str(),
                    layer.features.size());
                for (const auto& reason : reasons) {
                    ImGui::BulletText("%s", reason.c_str());
                }
                if (gates.empty()) {
                    ImGui::TextDisabled("No active feature-filter gates currently target this layer.");
                } else {
                    ImGui::Text("Active gates:");
                    for (const auto& gate : gates) {
                        ImGui::BulletText("%s", gate.c_str());
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}
}

void drawActiveQueriesTab(const ActiveQueriesTabContext& ctx) {
    if (!ImGui::BeginTabItem("Active Queries")) return;

    if (!ctx.map_filter_state || !ctx.query_layers || !ctx.active_result_set || !ctx.active_result_status || !ctx.layers) {
        ImGui::TextDisabled("Active Queries tab context is incomplete.");
        ImGui::EndTabItem();
        return;
    }

    drawPredicateStack(ctx);

    ImGui::SeparatorText("UI Filter State");
    const MapFilterState& filters = *ctx.map_filter_state;
    ImGui::Text("Enabled: %s", filters.enabled ? "true" : "false");
    if (filters.use_date) {
        ImGui::BulletText("Record year: %d-%d", filters.year_min, filters.year_max);
    }
    drawOptionalFilterField("Address", filters.address);
    drawOptionalFilterField("Block/Lot", filters.blocklot);
    drawOptionalFilterField("Status", filters.status);
    drawOptionalFilterField("Owner", filters.owner);
    drawOptionalFilterField("ZIP", filters.zip);
    if (!filters.selected_owners.empty()) {
        ImGui::BulletText("Selected owners: %zu", filters.selected_owners.size());
    }
    if (filters.crime.enabled) {
        ImGui::BulletText("Crime filter active%s", filters.crime.use_year ? " with year range" : "");
        if (filters.crime.use_year) {
            ImGui::BulletText("Crime years: %d-%d", filters.crime.year_min, filters.crime.year_max);
        }
    }

    ImGui::SeparatorText("Active Result Filter");
    if (!ctx.active_result_status->empty()) {
        ImGui::TextWrapped("%s", ctx.active_result_status->c_str());
    }
    drawResultSetSummary("Current renderer filter result set", *ctx.active_result_set);

    ImGui::SeparatorText("Query Layers");
    if (ctx.query_layers->empty()) {
        ImGui::TextDisabled("No SQL or API query layers are active.");
    } else {
        for (size_t i = 0; i < ctx.query_layers->size(); ++i) {
            QueryMapLayer& layer = (*ctx.query_layers)[i];
            ImGui::PushID((int)i);
            ImGui::Checkbox("##enabled", &layer.enabled);
            ImGui::SameLine();
            ImGui::ColorButton(
                "##color",
                ImVec4(layer.color[0], layer.color[1], layer.color[2], layer.color[3]),
                ImGuiColorEditFlags_NoTooltip,
                ImVec2(12.0f, 12.0f));
            ImGui::SameLine();
            ImGui::Text("%s", layer.name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                ctx.query_layers->erase(ctx.query_layers->begin() + (ptrdiff_t)i);
                ImGui::PopID();
                break;
            }
            ImGui::TextDisabled("%s", layer.status.c_str());
            ImGui::BulletText("rows=%zu", layer.row_count);
            drawResultSetSummary("result set", layer.result_set);
            if (!layer.sql.empty() && ImGui::TreeNode("SQL")) {
                ImGui::TextWrapped("%s", layer.sql.c_str());
                ImGui::TreePop();
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        if (ImGui::Button("Clear Query Layers")) {
            ctx.query_layers->clear();
        }
    }

    drawEnabledLayersSummary(*ctx.layers);
    drawZoningPredicateSummary(ctx);
    drawRenderGateSummary(ctx);
    drawPerLayerReasonSummary(ctx);

    ImGui::EndTabItem();
}
