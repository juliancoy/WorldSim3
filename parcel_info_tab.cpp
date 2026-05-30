#include "parcel_info_tab.h"

#include "app_utils.h"
#include "feature_props.h"
#include "imgui.h"
#include "parcel_value_ui.h"
#include "real_property_ui.h"

namespace {
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
    const std::string& parcel_entity_id) {
    DuckDbParcelDetailSnapshot out;
    if (!duckdb_analytics || !duckdb_analytics->status().last_rebuild_ok) return out;
    const DuckDbQueryResult detail = duckdb_analytics->queryUnifiedParcelDetail(parcel_entity_id);
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

bool drawDuckDbParcelDetail(OwnerInfoUiState* owner_info_state, DuckDbAnalytics* duckdb_analytics, const std::string& parcel_entity_id) {
    if (!duckdb_analytics || !duckdb_analytics->status().last_rebuild_ok) return false;
    const DuckDbQueryResult detail = duckdb_analytics->queryUnifiedParcelDetail(parcel_entity_id);
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
    if (owner_info_state) {
        drawSourceInfoLink(*owner_info_state, "Parcel Source:", cell("parcel_source_file"), false, "parcel_info_duckdb_parcel_source");
        drawSourceInfoLink(*owner_info_state, "Property Source:", cell("property_source_file"), true, "parcel_info_duckdb_property_source");
    } else {
        text_prop("Parcel Source", "parcel_source_file");
        text_prop("Property Source", "property_source_file");
    }
    ImGui::TextDisabled("Source: DuckDB unified_parcels (harmonized)");
    return true;
}

void drawUnifiedParcelDetail(OwnerInfoUiState* owner_info_state, const UnifiedParcelRecord& rec) {
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
    if (owner_info_state) {
        drawSourceInfoLink(*owner_info_state, "Parcel Source:", rec.parcel_source_file, false, "parcel_info_unified_parcel_source");
        drawSourceInfoLink(*owner_info_state, "Property Source:", rec.property_source_file, true, "parcel_info_unified_property_source");
    } else {
        text_prop("Parcel Source", rec.parcel_source_file);
        text_prop("Property Source", rec.property_source_file);
    }
    ImGui::TextDisabled("Source: in-memory unified parcel record");
}

}

void drawParcelInfoTab(const ParcelInfoTabContext& ctx) {
    ImGuiTabItemFlags tab_flags = (ctx.tab_requested && *ctx.tab_requested) ? ImGuiTabItemFlags_SetSelected : 0;
    if (!ImGui::BeginTabItem("Parcel Info", nullptr, tab_flags)) return;
    if (ctx.tab_requested) *ctx.tab_requested = false;

    const bool parcel_info_valid =
        ctx.show_selected_parcel_details &&
        ctx.parcel_selection &&
        !ctx.parcel_selection->refs.empty() &&
        ctx.layers &&
        ctx.parcel_selection->active_layer_idx >= 0 &&
        (size_t)ctx.parcel_selection->active_layer_idx < ctx.layers->size() &&
        !ctx.parcel_selection->active_entity_id.empty();

    if (!parcel_info_valid) {
        ImGui::TextDisabled("Select a parcel from the map or address results to view parcel details.");
    } else {
        if (ImGui::Button("Clear Parcel Selection")) {
            if (ctx.clear_parcel_selection) ctx.clear_parcel_selection();
        } else {
            const int active_layer_idx = ctx.parcel_selection->active_layer_idx;
            const std::string active_feature_id = ctx.parcel_selection->active_entity_id;
            const UnifiedParcelRecord* selected_unified = ctx.unified_parcels
                ? unifiedParcelAt(*ctx.unified_parcels, active_feature_id)
                : nullptr;
            const DuckDbParcelDetailSnapshot duckdb_detail =
                loadDuckDbParcelDetailSnapshot(ctx.duckdb_analytics, active_feature_id);
            std::string blocklot_raw = selected_unified ? selected_unified->blocklot : std::string();
            int vac_notice = 0;
            int vac_rehab = 0;
            int tax_lien = 0;
            int tax_sale = 0;
            double tax_lien_amount = 0.0;
            double tax_sale_amount = 0.0;
            double current_value_total = 0.0;
            if (ctx.unified_parcels) {
                for (const ParcelSelectionRef& ref : ctx.parcel_selection->refs) {
                    const UnifiedParcelRecord* u = unifiedParcelAt(*ctx.unified_parcels, ref.entity_id);
                    if (!u) continue;
                    vac_notice += u->vacant_notice_count;
                    vac_rehab += u->vacant_rehab_count;
                    tax_lien += u->tax_lien_count;
                    tax_sale += u->tax_sale_count;
                    tax_lien_amount += u->tax_lien_amount;
                    tax_sale_amount += u->tax_sale_amount;
                    current_value_total += u->current_value;
                }
            }
            if (ctx.parcel_selection->refs.size() == 1 && duckdb_detail.ok) {
                blocklot_raw = duckdb_detail.blocklot.empty() ? blocklot_raw : duckdb_detail.blocklot;
                vac_notice = duckdb_detail.vacant_notice_count;
                vac_rehab = duckdb_detail.vacant_rehab_count;
                tax_lien = duckdb_detail.tax_lien_count;
                tax_sale = duckdb_detail.tax_sale_count;
                tax_lien_amount = duckdb_detail.tax_lien_amount;
                tax_sale_amount = duckdb_detail.tax_sale_amount;
                current_value_total = duckdb_detail.current_value;
            }
            const LayerDef::FeatureRecord* selected_rp =
                (selected_unified && ctx.layers) ? unifiedRealPropertyGeometry(*selected_unified, *ctx.layers) : nullptr;
            if (!selected_rp && ctx.layers) {
                selected_rp = resolveRealPropertyForBlocklot(
                    *ctx.layers,
                    ctx.real_property_layer_idx,
                    ctx.real_property_by_blocklot,
                    blocklot_raw);
            }

            ImGui::Separator();
            ImGui::Text("Selected Parcels: %zu", ctx.parcel_selection->refs.size());
            ImGui::Text("Active BLOCKLOT: %s", blocklot_raw.empty() ? "(none)" : blocklot_raw.c_str());
            ImGui::Text("Vacant Notices: %d", vac_notice);
            ImGui::Text("Vacant Rehab Records: %d", vac_rehab);
            ImGui::Text("Tax Lien Certificate Records: %d", tax_lien);
            if (tax_lien > 0) ImGui::Text("Tax Lien Total Amount: %s", formatUsd(tax_lien_amount, 2).c_str());
            ImGui::Text("Tax Sale 2021 Records: %d", tax_sale);
            if (tax_sale > 0) ImGui::Text("Tax Sale Total Lien: %s", formatUsd(tax_sale_amount, 2).c_str());
            drawParcelCurrentValueTotal(current_value_total, selected_unified);

            std::string summary_owner =
                selected_unified
                    ? trimDisplayValue(selected_unified->owner_display.empty() ? selected_unified->owner : selected_unified->owner_display)
                    : std::string();
            if (summary_owner.empty() && selected_rp) {
                summary_owner = trimDisplayValue(firstDisplayProperty(
                    *selected_rp,
                    {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"}));
            }
            if (!summary_owner.empty() && ctx.owner_info_state) {
                ImGui::TextUnformatted("Owner:");
                ImGui::SameLine();
                ImGui::PushID("open_owner_info_parcel_tab");
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.38f, 0.72f, 0.10f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.40f, 0.72f, 0.22f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.34f, 0.62f, 0.32f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.72f, 1.00f, 1.0f));
                if (ImGui::Button(summary_owner.c_str())) {
                    openOwnerInfoPageAndSelectOwnerParcels(
                        *ctx.owner_info_state,
                        summary_owner,
                        ctx.unified_parcels,
                        ctx.clear_parcel_selection,
                        ctx.select_parcel_id);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Open owner in Element tab and select all parcels for this owner");
                }
                ImVec2 link_min = ImGui::GetItemRectMin();
                ImVec2 link_max = ImGui::GetItemRectMax();
                const float pad_x = ImGui::GetStyle().FramePadding.x;
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(link_min.x + pad_x, link_max.y - 3.0f),
                    ImVec2(link_max.x - pad_x, link_max.y - 3.0f),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(0.42f, 0.72f, 1.00f, 1.0f)),
                    1.0f);
                ImGui::PopStyleColor(4);
                ImGui::PopID();
            }
            if (duckdb_detail.ok) {
                drawDuckDbParcelDetail(ctx.owner_info_state, ctx.duckdb_analytics, active_feature_id);
            } else if (selected_unified) {
                drawUnifiedParcelDetail(ctx.owner_info_state, *selected_unified);
            } else if (!drawDuckDbParcelDetail(ctx.owner_info_state, ctx.duckdb_analytics, active_feature_id)) {
                drawRealPropertySummary(selected_rp);
            }
        }
    }
    ImGui::EndTabItem();
}
