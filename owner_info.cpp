#include "owner_info.h"

#include "app_utils.h"
#include "feature_props.h"
#include "imgui.h"
#include "parcel_value_ui.h"
#include "parcel_timeline.h"

#include <algorithm>

namespace {
ElementInfoEntry currentEntry(const ElementInfoUiState& state) {
    if (state.history_index >= state.history.size()) return {};
    return state.history[state.history_index];
}

bool sameEntry(const ElementInfoEntry& a, const ElementInfoEntry& b) {
    return a.kind == b.kind && a.parcel_idx == b.parcel_idx && a.owner == b.owner && a.source == b.source;
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

struct DuckDbParcelDetailSnapshot {
    bool ok = false;
    std::string blocklot;
    int vacant_notice_count = 0;
    int vacant_rehab_count = 0;
    int tax_lien_count = 0;
    int tax_sale_count = 0;
    double tax_lien_amount = 0.0;
    double tax_sale_amount = 0.0;
    double current_value = 0.0;
};

DuckDbParcelDetailSnapshot loadDuckDbParcelDetailSnapshot(
    DuckDbAnalytics* duckdb_analytics,
    int parcel_layer_idx,
    size_t parcel_feature_idx) {
    DuckDbParcelDetailSnapshot out;
    if (!duckdb_analytics || !duckdb_analytics->status().last_rebuild_ok) return out;
    const DuckDbQueryResult detail = duckdb_analytics->queryUnifiedParcelDetail((size_t)parcel_layer_idx, parcel_feature_idx);
    if (!detail.ok || detail.rows.empty()) return out;
    const auto& row = detail.rows.front();
    auto cell = [&](const char* column) -> std::string {
        for (size_t i = 0; i < detail.columns.size() && i < row.size(); ++i) {
            if (detail.columns[i] == column) return row[i];
        }
        return {};
    };
    out.ok = true;
    out.blocklot = cell("blocklot");
    out.vacant_notice_count = (int)parseNumericField(cell("vacant_notice_count"));
    out.vacant_rehab_count = (int)parseNumericField(cell("vacant_rehab_count"));
    out.tax_lien_count = (int)parseNumericField(cell("tax_lien_count"));
    out.tax_sale_count = (int)parseNumericField(cell("tax_sale_count"));
    out.tax_lien_amount = parseNumericField(cell("tax_lien_amount"));
    out.tax_sale_amount = parseNumericField(cell("tax_sale_amount"));
    out.current_value = parseNumericField(cell("current_value"));
    return out;
}

void drawSourceButton(ElementInfoUiState* state, const char* label, const std::string& source, bool property_source, const char* id) {
    if (source.empty() || !state) return;
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.05f, 0.22f, 0.55f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.05f, 0.28f, 0.72f, 0.18f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.04f, 0.20f, 0.58f, 0.28f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.30f, 0.78f, 1.0f));
    if (ImGui::Button(source.c_str())) {
        if (property_source) openPropertySourceInfoPage(*state, source);
        else openParcelSourceInfoPage(*state, source);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Open parcels filtered by this source");
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

bool drawDuckDbParcelDetail(ElementInfoUiState* state, DuckDbAnalytics* duckdb_analytics, int parcel_layer_idx, size_t parcel_feature_idx) {
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
    drawSourceButton(state, "Parcel Source:", cell("parcel_source_file"), false, "duckdb_parcel_source");
    drawSourceButton(state, "Property Source:", cell("property_source_file"), true, "duckdb_property_source");
    ImGui::TextDisabled("Source: DuckDB unified_parcels (harmonized)");
    return true;
}

void drawUnifiedParcelDetail(ElementInfoUiState* state, const UnifiedParcelRecord& rec) {
    auto text_prop = [&](const char* label, const std::string& value) {
        if (!value.empty()) ImGui::TextWrapped("%s: %s", label, value.c_str());
    };
    auto numeric_prop = [&](const char* label, double value) {
        if (value > 0.0) ImGui::TextWrapped("%s: %s", label, formatUsd(value, 2).c_str());
    };
    text_prop("Address", rec.address);
    text_prop("Owner", rec.owner_display.empty() ? rec.owner : rec.owner_display);
    text_prop("BLOCKLOT", rec.blocklot);
    text_prop("ZIP", rec.zip);
    text_prop("Status", rec.status);
    numeric_prop("Current Land", rec.current_land);
    numeric_prop("Current Improvements", rec.current_improvements);
    numeric_prop("Tax Base", rec.tax_base);
    numeric_prop("Sale Price", rec.sale_price);
    drawParcelCurrentValueDetail(rec);
    ImGui::Text("Vacant Notices: %d", rec.vacant_notice_count);
    ImGui::Text("Vacant Rehab Records: %d", rec.vacant_rehab_count);
    ImGui::Text("Tax Lien Records: %d", rec.tax_lien_count);
    ImGui::Text("Tax Sale Records: %d", rec.tax_sale_count);
    numeric_prop("Tax Lien Amount", rec.tax_lien_amount);
    numeric_prop("Tax Sale Amount", rec.tax_sale_amount);
    drawSourceButton(state, "Parcel Source:", rec.parcel_source_file, false, "unified_parcel_source");
    drawSourceButton(state, "Property Source:", rec.property_source_file, true, "unified_property_source");
    ImGui::TextDisabled("Source: in-memory unified parcel record");
}

bool drawDuckDbParcelTimeline(DuckDbAnalytics* duckdb_analytics, const std::string& blocklot) {
    if (!duckdb_analytics || !duckdb_analytics->status().last_rebuild_ok || trimDisplayValue(blocklot).empty()) return false;
    const DuckDbQueryResult result = duckdb_analytics->queryParcelEvents(blocklot, 256);
    if (!result.ok || result.rows.empty()) return false;
    auto cell = [&](const std::vector<std::string>& row, const char* column) -> std::string {
        for (size_t i = 0; i < result.columns.size() && i < row.size(); ++i) {
            if (result.columns[i] == column) return row[i];
        }
        return {};
    };
    ImGui::TextDisabled("%zu event(s), newest first", result.rows.size());
    ImGui::BeginChild("parcel_history_events", ImVec2(0, 260.0f), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (const auto& row : result.rows) {
        const std::string date = trimDisplayValue(cell(row, "event_date"));
        const std::string year = trimDisplayValue(cell(row, "event_year"));
        const std::string event_type = trimDisplayValue(cell(row, "event_type"));
        const std::string status = trimDisplayValue(cell(row, "event_status"));
        const std::string amount = trimDisplayValue(cell(row, "amount_usd"));
        const std::string source_name = trimDisplayValue(cell(row, "source_layer_name"));
        const std::string source_file = trimDisplayValue(cell(row, "source_layer_file"));
        const std::string date_label =
            !date.empty() && date != "NULL" ? date : (!year.empty() && year != "NULL" ? year : "(date unavailable)");
        ImGui::TextWrapped("%s - %s", date_label.c_str(), event_type.empty() || event_type == "NULL" ? "Event" : event_type.c_str());
        if (!status.empty() && status != "NULL") ImGui::TextWrapped("Status: %s", status.c_str());
        if (!amount.empty() && amount != "NULL") {
            const double amount_value = parseNumericField(amount);
            if (amount_value > 0.0) ImGui::TextWrapped("Amount: %s", formatUsd(amount_value, 2).c_str());
            else ImGui::TextWrapped("Amount: %s", amount.c_str());
        }
        if (!source_name.empty() && source_name != "NULL") ImGui::TextDisabled("Source: %s", source_name.c_str());
        else if (!source_file.empty() && source_file != "NULL") ImGui::TextDisabled("Source: %s", source_file.c_str());
        ImGui::Separator();
    }
    ImGui::EndChild();
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
    const DuckDbParcelDetailSnapshot duckdb_detail =
        loadDuckDbParcelDetailSnapshot(ctx.duckdb_analytics, ctx.parcel_layer_idx, parcel_idx);
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
    if (!(ctx.show_selected_parcel_details && ctx.selected_parcel_indices && ctx.selected_parcel_indices->size() > 1) &&
        duckdb_detail.ok) {
        blocklot_raw = duckdb_detail.blocklot.empty() ? blocklot_raw : duckdb_detail.blocklot;
        vac_notice = duckdb_detail.vacant_notice_count;
        vac_rehab = duckdb_detail.vacant_rehab_count;
        tax_lien = duckdb_detail.tax_lien_count;
        tax_sale = duckdb_detail.tax_sale_count;
        tax_lien_amount = duckdb_detail.tax_lien_amount;
        tax_sale_amount = duckdb_detail.tax_sale_amount;
        current_value_total = duckdb_detail.current_value;
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
    drawParcelCurrentValueTotal(current_value_total, selected_unified);

    std::string summary_owner = selected_unified ? selected_unified->owner : ownerNameFor(selected_rp);
    if (summary_owner.empty()) summary_owner = ownerNameFor(&selected);
    if (!summary_owner.empty() && ctx.state) drawOwnerInfoLink(*ctx.state, summary_owner, "open_owner_info_element_tab");
    if (duckdb_detail.ok) {
        drawDuckDbParcelDetail(ctx.state, ctx.duckdb_analytics, ctx.parcel_layer_idx, parcel_idx);
    } else if (selected_unified) {
        drawUnifiedParcelDetail(ctx.state, *selected_unified);
    } else if (!drawDuckDbParcelDetail(ctx.state, ctx.duckdb_analytics, ctx.parcel_layer_idx, parcel_idx)) {
        drawRealPropertySummary(selected_rp);
    }

    ImGui::SeparatorText("Parcel History");
    if (!drawDuckDbParcelTimeline(ctx.duckdb_analytics, duckdb_detail.ok ? duckdb_detail.blocklot : blocklot_raw)) {
        if (!ctx.duckdb_analytics || !ctx.duckdb_analytics->status().last_rebuild_ok) {
            ImGui::TextDisabled("Parcel history requires DuckDB analytics.");
        } else {
            ImGui::TextDisabled("No parcel history events found in DuckDB for this parcel.");
        }
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

void drawSourceElement(const OwnerInfoTabContext& ctx, const std::string& source, bool property_source) {
    if (source.empty()) {
        ImGui::TextDisabled("Source is unavailable.");
        return;
    }
    if (!ctx.unified_parcels || !ctx.layers || ctx.parcel_layer_idx < 0 || (size_t)ctx.parcel_layer_idx >= ctx.layers->size()) {
        ImGui::TextDisabled("Source parcel data is unavailable.");
        return;
    }

    std::vector<size_t> source_parcel_indices;
    source_parcel_indices.reserve(1024);
    double source_value_total = 0.0;
    for (const auto& parcel_record : *ctx.unified_parcels) {
        const std::string& candidate = property_source ? parcel_record.property_source_file : parcel_record.parcel_source_file;
        if (candidate != source) continue;
        source_parcel_indices.push_back(parcel_record.parcel_feature_idx);
        source_value_total += parcel_record.current_value;
    }

    ImGui::Text("Element: %s", property_source ? "Property Source" : "Parcel Source");
    ImGui::TextWrapped("Source: %s", source.c_str());
    ImGui::Text("Properties: %zu", source_parcel_indices.size());
    ImGui::Text("Total Current Value: %s", formatUsd(source_value_total).c_str());

    if (ctx.state && ctx.state->property_query && ctx.state->property_query_size > 0) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint(
            "##source_info_property_search",
            "Search properties by address or block/lot",
            ctx.state->property_query,
            ctx.state->property_query_size);
    }

    const std::string property_query = (ctx.state && ctx.state->property_query)
        ? trimDisplayValue(ctx.state->property_query)
        : std::string();
    size_t visible_source_properties = 0;
    ImGui::BeginChild("source_info_properties", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (size_t pi : source_parcel_indices) {
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

        visible_source_properties++;
        std::string label = address + "##source_prop_" + std::to_string(pi);
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
    if (visible_source_properties == 0) ImGui::TextDisabled("No matching properties.");
    ImGui::EndChild();
}
}

void openElementParcelPage(ElementInfoUiState& state, size_t parcel_idx) {
    openElementPage(state, ElementInfoEntry{ElementInfoKind::Parcel, parcel_idx, {}});
}

void openOwnerInfoPage(ElementInfoUiState& state, const std::string& owner) {
    openElementPage(state, ElementInfoEntry{ElementInfoKind::Owner, (size_t)-1, owner});
}

void openParcelSourceInfoPage(ElementInfoUiState& state, const std::string& source) {
    openElementPage(state, ElementInfoEntry{ElementInfoKind::ParcelSource, (size_t)-1, {}, source});
}

void openPropertySourceInfoPage(ElementInfoUiState& state, const std::string& source) {
    openElementPage(state, ElementInfoEntry{ElementInfoKind::PropertySource, (size_t)-1, {}, source});
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

void drawSourceInfoLink(ElementInfoUiState& state, const char* label, const std::string& source, bool property_source, const char* id) {
    drawSourceButton(&state, label, source, property_source, id);
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
    } else if (entry.kind == ElementInfoKind::ParcelSource) {
        drawSourceElement(ctx, entry.source, false);
    } else if (entry.kind == ElementInfoKind::PropertySource) {
        drawSourceElement(ctx, entry.source, true);
    } else {
        ImGui::TextDisabled("Open a parcel from the map/search or an owner from owner search.");
    }
    ImGui::EndTabItem();
}
