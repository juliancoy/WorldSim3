#include "owner_info.h"

#include "app_utils.h"
#include "feature_props.h"
#include "imgui.h"
#include "parcel_timeline.h"

#include <algorithm>

namespace {
ElementInfoEntry currentEntry(const ElementInfoUiState& state) {
    if (state.history_index >= state.history.size()) return {};
    return state.history[state.history_index];
}

bool sameEntry(const ElementInfoEntry& a, const ElementInfoEntry& b) {
    return a.kind == b.kind && a.parcel_idx == b.parcel_idx && a.owner == b.owner;
}

void clearOwnerPropertyQuery(ElementInfoUiState& state) {
    if (state.property_query && state.property_query_size > 0) state.property_query[0] = '\0';
}

void openElementPage(ElementInfoUiState& state, ElementInfoEntry entry) {
    if (entry.kind == ElementInfoKind::None) return;
    const ElementInfoEntry current = currentEntry(state);
    if (sameEntry(current, entry)) {
        state.tab_requested = true;
        return;
    }
    if (state.history_index + 1 < state.history.size()) {
        state.history.erase(state.history.begin() + (long)state.history_index + 1, state.history.end());
    }
    state.history.push_back(std::move(entry));
    state.history_index = state.history.size() - 1;
    state.tab_requested = true;
    if (currentEntry(state).kind == ElementInfoKind::Owner) clearOwnerPropertyQuery(state);
}

bool drawDuckDbParcelDetail(DuckDbAnalytics* duckdb_analytics, int parcel_layer_idx, size_t parcel_feature_idx) {
    if (!duckdb_analytics || !duckdb_analytics->status().last_rebuild_ok) return false;
    const DuckDbQueryResult detail = duckdb_analytics->queryUnifiedParcelDetail((size_t)parcel_layer_idx, parcel_feature_idx);
    if (!detail.ok || detail.rows.empty()) return false;
    const auto& row = detail.rows.front();
    auto cell = [&](const char* column) -> std::string {
        for (size_t i = 0; i < detail.columns.size() && i < row.size(); ++i) {
            if (detail.columns[i] == column) return row[i];
        }
        return {};
    };
    auto text_prop = [&](const char* label, const char* column) {
        const std::string value = cell(column);
        if (!value.empty() && value != "NULL") ImGui::TextWrapped("%s: %s", label, value.c_str());
    };
    text_prop("Address", "address");
    text_prop("Owner", "owner_display");
    text_prop("BLOCKLOT", "blocklot");
    text_prop("ZIP", "zipcode");
    text_prop("Status", "status");
    text_prop("Current Land", "current_land");
    text_prop("Current Improvements", "current_improvements");
    text_prop("Tax Base", "tax_base");
    text_prop("Sale Price", "sale_price");
    text_prop("Current Value", "current_value");
    text_prop("Vacant Notices", "vacant_notice_count");
    text_prop("Vacant Rehab Records", "vacant_rehab_count");
    text_prop("Tax Lien Records", "tax_lien_count");
    text_prop("Tax Sale Records", "tax_sale_count");
    text_prop("Tax Lien Amount", "tax_lien_amount");
    text_prop("Tax Sale Amount", "tax_sale_amount");
    text_prop("Parcel Source", "parcel_source_file");
    text_prop("Property Source", "property_source_file");
    ImGui::TextDisabled("Source: DuckDB unified_parcels (harmonized)");
    return true;
}

std::string ownerNameFor(const LayerDef::FeatureGeom* rp) {
    if (!rp) return "";
    std::string o = firstDisplayProperty(*rp, {"OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR"});
    return toLowerAscii(trimDisplayValue(o));
}

void drawRealPropertySummary(const LayerDef::FeatureGeom* rp) {
    if (!rp) {
        ImGui::TextDisabled("No matching real-property record.");
        return;
    }
    auto text_prop = [&](const char* label, const std::string& value) {
        if (!value.empty()) ImGui::TextWrapped("%s: %s", label, value.c_str());
    };
    text_prop("Address", firstDisplayProperty(*rp, {"FULLADDR", "PROPERTY_ADDRESS", "PREMISEADD", "ADDRESS", "Address", "ADDR"}));
    text_prop("Use", firstDisplayProperty(*rp, {"LU", "LANDUSE", "USE_CODE", "USE"}));
    text_prop("Tax Base", firstDisplayProperty(*rp, {"TAXBASE", "ARTAXBAS"}));
    text_prop("Current Land", firstDisplayProperty(*rp, {"CURRLAND"}));
    text_prop("Current Improvements", firstDisplayProperty(*rp, {"CURRIMPR"}));
    text_prop("Sale Price", firstDisplayProperty(*rp, {"SALEPRIC"}));
    text_prop("Sale Date", firstDisplayProperty(*rp, {"SALEDATE"}));
    std::string deed_book = firstDisplayProperty(*rp, {"DEEDBOOK"});
    std::string deed_page = firstDisplayProperty(*rp, {"DEEDPAGE"});
    text_prop("Deed", deed_book.empty() ? "" : deed_book + (deed_page.empty() ? "" : " / " + deed_page));
    text_prop("SDAT Link", firstDisplayProperty(*rp, {"SDATLINK"}));
    ImGui::TextDisabled("Source: Local property records when available");
}

void drawNavigation(ElementInfoUiState& state) {
    const bool can_back = state.history_index > 0 && state.history_index < state.history.size();
    const bool can_forward = state.history_index + 1 < state.history.size();
    ImGui::BeginDisabled(!can_back);
    if (ImGui::Button("< Back")) state.history_index--;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_forward);
    if (ImGui::Button("Forward >")) state.history_index++;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (state.history_index < state.history.size()) {
        ImGui::TextDisabled("%zu / %zu", state.history_index + 1, state.history.size());
    } else {
        ImGui::TextDisabled("0 / 0");
    }
    ImGui::Separator();
}

void drawParcelElement(const OwnerInfoTabContext& ctx, size_t parcel_idx) {
    const bool parcel_info_valid =
        ctx.layers &&
        ctx.parcel_layer_idx >= 0 &&
        (size_t)ctx.parcel_layer_idx < ctx.layers->size() &&
        parcel_idx < (*ctx.layers)[(size_t)ctx.parcel_layer_idx].features.size();

    if (!parcel_info_valid) {
        ImGui::TextDisabled("Parcel is unavailable.");
        return;
    }

    const auto& selected = (*ctx.layers)[(size_t)ctx.parcel_layer_idx].features[parcel_idx];
    if (ImGui::Button("Clear Parcel Selection")) {
        if (ctx.clear_parcel_selection) ctx.clear_parcel_selection();
    }

    const UnifiedParcelRecord* selected_unified = ctx.unified_parcels
        ? unifiedParcelAt(*ctx.unified_parcels, parcel_idx)
        : nullptr;
    std::string blocklot_raw = getPropertyValue(selected, "BLOCKLOT");
    int vac_notice = 0;
    int vac_rehab = 0;
    int tax_lien = 0;
    int tax_sale = 0;
    double tax_lien_amount = 0.0;
    double tax_sale_amount = 0.0;
    double current_value_total = 0.0;
    if (ctx.unified_parcels) {
        if (ctx.show_selected_parcel_details && ctx.selected_parcel_indices && !ctx.selected_parcel_indices->empty()) {
            for (size_t sel_idx : *ctx.selected_parcel_indices) {
                const UnifiedParcelRecord* u = unifiedParcelAt(*ctx.unified_parcels, sel_idx);
                if (!u) continue;
                vac_notice += u->vacant_notice_count;
                vac_rehab += u->vacant_rehab_count;
                tax_lien += u->tax_lien_count;
                tax_sale += u->tax_sale_count;
                tax_lien_amount += u->tax_lien_amount;
                tax_sale_amount += u->tax_sale_amount;
                current_value_total += u->current_value;
            }
        } else if (selected_unified) {
            vac_notice = selected_unified->vacant_notice_count;
            vac_rehab = selected_unified->vacant_rehab_count;
            tax_lien = selected_unified->tax_lien_count;
            tax_sale = selected_unified->tax_sale_count;
            tax_lien_amount = selected_unified->tax_lien_amount;
            tax_sale_amount = selected_unified->tax_sale_amount;
            current_value_total = selected_unified->current_value;
        }
    }

    const LayerDef::FeatureGeom* selected_rp = selected_unified ? selected_unified->real_property : nullptr;
    if (!selected_rp && ctx.real_property_for_parcel) selected_rp = ctx.real_property_for_parcel(selected);

    ImGui::Separator();
    const size_t selected_count = (ctx.show_selected_parcel_details && ctx.selected_parcel_indices)
        ? ctx.selected_parcel_indices->size()
        : 1;
    ImGui::Text("Element: Parcel");
    ImGui::Text("Selected Parcels: %zu", selected_count);
    ImGui::Text("Active BLOCKLOT: %s", blocklot_raw.empty() ? "(none)" : blocklot_raw.c_str());
    ImGui::Text("Vacant Notices: %d", vac_notice);
    ImGui::Text("Vacant Rehab Records: %d", vac_rehab);
    ImGui::Text("Tax Lien Certificate Records: %d", tax_lien);
    if (tax_lien > 0) ImGui::Text("Tax Lien Total Amount: %s", formatUsd(tax_lien_amount, 2).c_str());
    ImGui::Text("Tax Sale 2021 Records: %d", tax_sale);
    if (tax_sale > 0) ImGui::Text("Tax Sale Total Lien: %s", formatUsd(tax_sale_amount, 2).c_str());
    if (current_value_total > 0.0) ImGui::Text("Current Parcel Value (Total): %s", formatUsd(current_value_total, 2).c_str());

    std::string summary_owner = selected_unified ? selected_unified->owner : ownerNameFor(selected_rp);
    if (summary_owner.empty()) summary_owner = ownerNameFor(&selected);
    if (!summary_owner.empty() && ctx.state) drawOwnerInfoLink(*ctx.state, summary_owner, "open_owner_info_element_tab");
    if (!drawDuckDbParcelDetail(ctx.duckdb_analytics, ctx.parcel_layer_idx, parcel_idx)) {
        drawRealPropertySummary(selected_rp);
    }

    std::vector<ParcelTimelineEvent> timeline = buildParcelTimeline(ParcelTimelineRequest{
        ctx.layers,
        &selected,
        selected_rp,
        ctx.vacant_notice_layer_idx,
        ctx.vacant_rehab_layer_idx,
        ctx.tax_lien_layer_idx,
        ctx.tax_sale_layer_idx
    });
    ImGui::SeparatorText("Parcel History");
    if (timeline.empty()) {
        ImGui::TextDisabled("No parcel history events found in loaded layers.");
    } else {
        ImGui::TextDisabled("%zu event(s), newest first", timeline.size());
        ImGui::BeginChild("parcel_history_events", ImVec2(0, 260.0f), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        for (const auto& event : timeline) {
            ImGui::TextWrapped("%s - %s",
                event.date.empty() ? "(date unavailable)" : event.date.c_str(),
                event.event_type.empty() ? "Event" : event.event_type.c_str());
            if (!event.status.empty()) ImGui::TextWrapped("Status: %s", event.status.c_str());
            if (!event.amount.empty()) ImGui::TextWrapped("Amount: %s", event.amount.c_str());
            if (!event.source_layer.empty()) ImGui::TextDisabled("Source: %s", event.source_layer.c_str());
            ImGui::Separator();
        }
        ImGui::EndChild();
    }
}

void drawOwnerElement(const OwnerInfoTabContext& ctx, const std::string& owner) {
    if (owner.empty()) {
        ImGui::TextDisabled("Owner is unavailable.");
    } else if (!ctx.layers || ctx.parcel_layer_idx < 0 || (size_t)ctx.parcel_layer_idx >= ctx.layers->size()) {
        ImGui::TextDisabled("Parcel layer is unavailable.");
    } else if (!ctx.unified_parcels) {
        ImGui::TextDisabled("Owner parcel data is unavailable.");
    } else {
        std::vector<size_t> owner_parcel_indices;
        owner_parcel_indices.reserve(512);
        double owner_value_total = 0.0;
        for (const auto& parcel_record : *ctx.unified_parcels) {
            if (parcel_record.owner != owner) continue;
            owner_parcel_indices.push_back(parcel_record.parcel_feature_idx);
            owner_value_total += parcel_record.current_value;
        }

        ImGui::Text("Element: Owner");
        ImGui::Text("Owner: %s", owner.c_str());
        ImGui::Text("Properties: %zu", owner_parcel_indices.size());
        ImGui::Text("Total Current Value: %s", formatUsd(owner_value_total).c_str());

        if (ctx.state && ctx.state->property_query && ctx.state->property_query_size > 0) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint(
                "##owner_info_property_search",
                "Search properties by address or block/lot",
                ctx.state->property_query,
                ctx.state->property_query_size);
        }

        const std::string property_query = (ctx.state && ctx.state->property_query)
            ? trimDisplayValue(ctx.state->property_query)
            : std::string();
        size_t visible_owner_properties = 0;
        ImGui::BeginChild("owner_info_properties", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        for (size_t pi : owner_parcel_indices) {
            const UnifiedParcelRecord* parcel_record = unifiedParcelAt(*ctx.unified_parcels, pi);
            if (!parcel_record || !parcel_record->parcel_geom) continue;

            const auto& pf = *parcel_record->parcel_geom;
            std::string blocklot = parcel_record->blocklot;
            std::string address = parcel_record->address;
            if (address.empty()) address = "(address unavailable)";
            if (!property_query.empty() &&
                !containsCaseInsensitive(address, property_query) &&
                !containsCaseInsensitive(blocklot, property_query)) {
                continue;
            }

            visible_owner_properties++;
            std::string label = address + "##owner_prop_" + std::to_string(pi);
            const bool row_selected = ctx.selected_parcel_index_set &&
                ctx.selected_parcel_index_set->find(pi) != ctx.selected_parcel_index_set->end();
            if (ImGui::Selectable(label.c_str(), row_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                if (ctx.center_lon) *ctx.center_lon = ((double)pf.extent.min_lon + (double)pf.extent.max_lon) * 0.5;
                if (ctx.center_lat) {
                    *ctx.center_lat = std::clamp(((double)pf.extent.min_lat + (double)pf.extent.max_lat) * 0.5, -85.0, 85.0);
                }
                if (ctx.zoom) *ctx.zoom = std::max(*ctx.zoom, 18);
                if (ctx.select_parcel_idx) ctx.select_parcel_idx(pi, ImGui::GetIO().KeyCtrl);
                if (ctx.state) openElementParcelPage(*ctx.state, pi);
            }
            ImGui::TextDisabled("BLOCKLOT: %s", blocklot.empty() ? "(none)" : blocklot.c_str());
            if (parcel_record->current_value > 0.0) ImGui::TextDisabled("Current value: %s", formatUsd(parcel_record->current_value).c_str());
            ImGui::Separator();
        }
        if (visible_owner_properties == 0) ImGui::TextDisabled("No matching properties.");
        ImGui::EndChild();
    }
}
}

void openElementParcelPage(ElementInfoUiState& state, size_t parcel_idx) {
    openElementPage(state, ElementInfoEntry{ElementInfoKind::Parcel, parcel_idx, {}});
}

void openOwnerInfoPage(ElementInfoUiState& state, const std::string& owner) {
    openElementPage(state, ElementInfoEntry{ElementInfoKind::Owner, (size_t)-1, owner});
}

void drawOwnerInfoLink(ElementInfoUiState& state, const std::string& owner, const char* id) {
    ImGui::TextUnformatted("Owner:");
    ImGui::SameLine();
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.05f, 0.22f, 0.55f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.05f, 0.28f, 0.72f, 0.18f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.04f, 0.20f, 0.58f, 0.28f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.30f, 0.78f, 1.0f));
    if (ImGui::Button(owner.c_str())) openOwnerInfoPage(state, owner);
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Open owner in Element tab");
    }
    ImVec2 link_min = ImGui::GetItemRectMin();
    ImVec2 link_max = ImGui::GetItemRectMax();
    const float pad_x = ImGui::GetStyle().FramePadding.x;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(link_min.x + pad_x, link_max.y - 3.0f),
        ImVec2(link_max.x - pad_x, link_max.y - 3.0f),
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.05f, 0.30f, 0.78f, 1.0f)),
        1.0f);
    ImGui::PopStyleColor(4);
    ImGui::PopID();
}

void drawElementInfoTab(const OwnerInfoTabContext& ctx) {
    if (!ctx.state) return;
    ImGuiTabItemFlags tab_flags = ctx.state->tab_requested ? ImGuiTabItemFlags_SetSelected : 0;
    if (!ImGui::BeginTabItem("Element", nullptr, tab_flags)) return;
    ctx.state->tab_requested = false;

    drawNavigation(*ctx.state);
    const ElementInfoEntry entry = currentEntry(*ctx.state);
    if (entry.kind == ElementInfoKind::Parcel) {
        drawParcelElement(ctx, entry.parcel_idx);
    } else if (entry.kind == ElementInfoKind::Owner) {
        drawOwnerElement(ctx, entry.owner);
    } else {
        ImGui::TextDisabled("Open a parcel from the map/search or an owner from owner search.");
    }
    ImGui::EndTabItem();
}
