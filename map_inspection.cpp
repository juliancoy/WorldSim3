#include "map_inspection.h"

#include "app_utils.h"
#include "feature_props.h"
#include "imgui.h"
#include "layer_geometry.h"

namespace {
void drawFeatureProperties(const char* title, const LayerDef::FeatureGeom& fg) {
    ImGui::TextUnformatted(title);
    for (const auto& kv : fg.properties) {
        std::string v = trimDisplayValue(kv.second);
        if (v.empty()) continue;
        ImGui::TextWrapped("%s: %s", kv.first.c_str(), v.c_str());
    }
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

void handleMapInspection(const MapInspectionContext& ctx) {
    if (!ctx.hover_state || !ctx.layers || !ctx.parcel_selection) return;
    const LayerDef::FeatureGeom* hovered_parcel = ctx.hover_state->hovered_parcel;
    const size_t hovered_parcel_idx = ctx.hover_state->hovered_parcel_idx;
    const LayerDef::FeatureGeom* hovered_zone = ctx.hover_state->hovered_zone;
    const size_t hovered_zone_idx = ctx.hover_state->hovered_zone_idx;

    if (ctx.map_hovered && ctx.parcel_inspect_active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered_parcel != nullptr) {
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        if (ctx.parcel_layer_idx >= 0 && (size_t)ctx.parcel_layer_idx < ctx.layers->size()) {
            if (selectParcel(*ctx.parcel_selection, hovered_parcel_idx, (*ctx.layers)[(size_t)ctx.parcel_layer_idx].features.size(), ctrl) &&
                ctx.open_parcel_element) {
                ctx.open_parcel_element(hovered_parcel_idx);
            }
        }
        if (ctx.show_selected_zone_details) *ctx.show_selected_zone_details = false;
        if (ctx.selected_zone_idx) *ctx.selected_zone_idx = (size_t)-1;
    } else if (ctx.map_hovered && ctx.zoning_inspect_active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered_zone != nullptr) {
        if (ctx.show_selected_zone_details) *ctx.show_selected_zone_details = true;
        if (ctx.selected_zone_idx) *ctx.selected_zone_idx = hovered_zone_idx;
        clearParcelSelection(*ctx.parcel_selection);
    }

    if (ctx.parcel_hover_active && ctx.map_hovered && hovered_parcel) {
        std::string blocklot_raw = getPropertyValue(*hovered_parcel, "BLOCKLOT");
        const auto& vac_notice_vec = ctx.parcel_vac_notice_by_feature ? *ctx.parcel_vac_notice_by_feature : std::vector<int>{};
        const auto& vac_rehab_vec = ctx.parcel_vac_rehab_by_feature ? *ctx.parcel_vac_rehab_by_feature : std::vector<int>{};
        const auto& tax_lien_vec = ctx.parcel_tax_lien_by_feature ? *ctx.parcel_tax_lien_by_feature : std::vector<int>{};
        const auto& tax_sale_vec = ctx.parcel_tax_sale_by_feature ? *ctx.parcel_tax_sale_by_feature : std::vector<int>{};
        const auto& tax_lien_amount_vec = ctx.parcel_tax_lien_amount_by_feature ? *ctx.parcel_tax_lien_amount_by_feature : std::vector<double>{};
        const auto& tax_sale_amount_vec = ctx.parcel_tax_sale_amount_by_feature ? *ctx.parcel_tax_sale_amount_by_feature : std::vector<double>{};
        int vac_notice = (hovered_parcel_idx < vac_notice_vec.size()) ? vac_notice_vec[hovered_parcel_idx] : 0;
        int vac_rehab = (hovered_parcel_idx < vac_rehab_vec.size()) ? vac_rehab_vec[hovered_parcel_idx] : 0;
        int tax_lien = (hovered_parcel_idx < tax_lien_vec.size()) ? tax_lien_vec[hovered_parcel_idx] : 0;
        int tax_sale = (hovered_parcel_idx < tax_sale_vec.size()) ? tax_sale_vec[hovered_parcel_idx] : 0;
        double tax_lien_amount = (hovered_parcel_idx < tax_lien_amount_vec.size()) ? tax_lien_amount_vec[hovered_parcel_idx] : 0.0;
        double tax_sale_amount = (hovered_parcel_idx < tax_sale_amount_vec.size()) ? tax_sale_amount_vec[hovered_parcel_idx] : 0.0;
        const LayerDef::FeatureGeom* hovered_zoning = hovered_zone;
        if (ctx.zoning_layer_idx >= 0 && (size_t)ctx.zoning_layer_idx < ctx.layers->size()) {
            const float qlon = (hovered_parcel->extent.min_lon + hovered_parcel->extent.max_lon) * 0.5f;
            const float qlat = (hovered_parcel->extent.min_lat + hovered_parcel->extent.max_lat) * 0.5f;
            std::vector<uint32_t> zoning_candidates;
            bool have_zoning_candidates = false;
            if (ctx.layer_spatial && (size_t)ctx.zoning_layer_idx < ctx.layer_spatial->size() && (*ctx.layer_spatial)[(size_t)ctx.zoning_layer_idx].built) {
                have_zoning_candidates = queryLayerSpatialIndex(
                    (*ctx.layer_spatial)[(size_t)ctx.zoning_layer_idx], qlon, qlat, qlon, qlat, zoning_candidates);
            }
            if (have_zoning_candidates) {
                const auto& zfeats = (*ctx.layers)[(size_t)ctx.zoning_layer_idx].features;
                for (uint32_t zi : zoning_candidates) {
                    if (zi >= zfeats.size()) continue;
                    const auto& zf = zfeats[zi];
                    if (qlon >= zf.extent.min_lon && qlon <= zf.extent.max_lon &&
                        qlat >= zf.extent.min_lat && qlat <= zf.extent.max_lat &&
                        pointInFeature(zf, qlon, qlat)) {
                        hovered_zoning = &zf;
                        break;
                    }
                }
            }
        }

        ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(440.0f);
        ImGui::TextUnformatted("Parcel Details");
        ImGui::Separator();
        ImGui::Text("BLOCKLOT: %s", blocklot_raw.empty() ? "(none)" : blocklot_raw.c_str());
        ImGui::Text("Vacant Notices: %d", vac_notice);
        ImGui::Text("Vacant Rehab Records: %d", vac_rehab);
        ImGui::Text("Tax Lien Certificate Records: %d", tax_lien);
        if (tax_lien > 0) ImGui::Text("Tax Lien Total Amount: %s", formatUsd(tax_lien_amount, 2).c_str());
        ImGui::Text("Tax Sale 2021 Records: %d", tax_sale);
        if (tax_sale > 0) ImGui::Text("Tax Sale Total Lien: %s", formatUsd(tax_sale_amount, 2).c_str());

        const LayerDef::FeatureGeom* hovered_rp = ctx.real_property_for_parcel ? ctx.real_property_for_parcel(*hovered_parcel) : nullptr;
        drawRealPropertySummary(hovered_rp);

        ImGui::Separator();
        if (hovered_zoning && ctx.zoning_metadata) {
            std::string zone_key = zoningClassKey(*hovered_zoning);
            std::string zone_label = zoningClassLabel(*hovered_zoning);
            auto meta_it = ctx.zoning_metadata->find(zone_key);
            if (meta_it != ctx.zoning_metadata->end() && !meta_it->second.label.empty()) zone_label = meta_it->second.label;
            std::string zone_description = zoningDescription(*hovered_zoning, *ctx.zoning_metadata);
            const char* display_zone = !zone_label.empty() ? zone_label.c_str() : (zone_key.empty() ? "(unlabeled)" : zone_key.c_str());
            ImGui::SetWindowFontScale(1.45f);
            ImGui::TextWrapped("%s", display_zone);
            ImGui::SetWindowFontScale(1.0f);
            if (!zone_description.empty()) ImGui::TextWrapped("%s", zone_description.c_str());
        } else if (ctx.zoning_layer_idx >= 0) {
            ImGui::TextDisabled("Zoning: no intersecting zoning polygon found.");
        }
        ImGui::Separator();
        drawFeatureProperties("All Parcel Geometry Fields", *hovered_parcel);
        if (hovered_rp) {
            ImGui::Separator();
            drawFeatureProperties("All Real Property Fields", *hovered_rp);
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (ctx.zoning_hover_active && ctx.map_hovered && !(ctx.parcel_hover_active && hovered_parcel) && hovered_zone && ctx.zoning_metadata) {
        drawZoningHoverTooltip(*hovered_zone, *ctx.zoning_metadata);
    }
}
