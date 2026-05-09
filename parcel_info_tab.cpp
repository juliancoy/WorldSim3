#include "parcel_info_tab.h"

#include "app_utils.h"
#include "feature_props.h"
#include "imgui.h"

namespace {
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
    text_prop("Owner", firstDisplayProperty(*rp, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"}));
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
}

void drawParcelInfoTab(const ParcelInfoTabContext& ctx) {
    ImGuiTabItemFlags tab_flags = (ctx.tab_requested && *ctx.tab_requested) ? ImGuiTabItemFlags_SetSelected : 0;
    if (!ImGui::BeginTabItem("Parcel Info", nullptr, tab_flags)) return;
    if (ctx.tab_requested) *ctx.tab_requested = false;

    const bool parcel_info_valid =
        ctx.show_selected_parcel_details &&
        ctx.selected_parcel_indices &&
        !ctx.selected_parcel_indices->empty() &&
        ctx.layers &&
        ctx.parcel_layer_idx >= 0 &&
        (size_t)ctx.parcel_layer_idx < ctx.layers->size() &&
        ctx.selected_parcel_idx < (*ctx.layers)[(size_t)ctx.parcel_layer_idx].features.size();

    if (!parcel_info_valid) {
        ImGui::TextDisabled("Select a parcel from the map or address results to view parcel details.");
    } else {
        const auto& selected = (*ctx.layers)[(size_t)ctx.parcel_layer_idx].features[ctx.selected_parcel_idx];
        if (ImGui::Button("Clear Parcel Selection")) {
            if (ctx.clear_parcel_selection) ctx.clear_parcel_selection();
        } else {
            const UnifiedParcelRecord* selected_unified = ctx.unified_parcels
                ? unifiedParcelAt(*ctx.unified_parcels, ctx.selected_parcel_idx)
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
            }
            const LayerDef::FeatureGeom* selected_rp = selected_unified ? selected_unified->real_property : nullptr;
            if (!selected_rp && ctx.real_property_for_parcel) selected_rp = ctx.real_property_for_parcel(selected);

            ImGui::Separator();
            ImGui::Text("Selected Parcels: %zu", ctx.selected_parcel_indices->size());
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
            if (!summary_owner.empty() && ctx.owner_info_state) {
                drawOwnerInfoLink(*ctx.owner_info_state, summary_owner, "open_owner_info_parcel_tab");
            }
            drawRealPropertySummary(selected_rp);
        }
    }
    ImGui::EndTabItem();
}
