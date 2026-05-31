#include "parcel_info_tab.h"

#include "app_utils.h"
#include "feature_props.h"
#include "imgui.h"
#include "net_http_utils.h"
#include "parcel_value_ui.h"
#include "real_property_ui.h"

#include <cstdio>
#include <string>

namespace {
struct DuckDbParcelDetailSnapshot {
    bool ok = false;
    std::string blocklot;
    std::string address;
    std::string owner_display;
    std::string zip;
    LayerDef::FeatureExtent parcel_extent;
    int vacant_notice_count = 0;
    int vacant_rehab_count = 0;
    int tax_lien_count = 0;
    int tax_sale_count = 0;
    int foreclosure_filing_count = 0;
    int open_receivership_count = 0;
    int auction_count = 0;
    double tax_lien_amount = 0.0;
    double tax_sale_amount = 0.0;
    double current_value = 0.0;
    std::string latest_foreclosure_date;
    std::string next_auction_date;
    std::string last_sold_at_auction_date;
};

DuckDbParcelDetailSnapshot loadDuckDbParcelDetailSnapshot(
    DuckDbAnalytics* duckdb_analytics,
    const std::string& parcel_entity_id) {
    DuckDbParcelDetailSnapshot out;
    if (!duckdb_analytics || !duckdb_analytics->ensureReady()) return out;
    const DuckDbUnifiedParcelDetail detail = duckdb_analytics->queryUnifiedParcelDetailRecord(parcel_entity_id);
    if (!detail.ok || !detail.found) return out;
    const UnifiedParcelRecord& rec = detail.record;
    out.ok = true;
    out.blocklot = rec.blocklot;
    out.address = rec.address;
    out.owner_display = rec.owner_display.empty() ? rec.owner : rec.owner_display;
    out.zip = rec.zip;
    out.parcel_extent = rec.parcel_extent;
    out.vacant_notice_count = rec.vacant_notice_count;
    out.vacant_rehab_count = rec.vacant_rehab_count;
    out.tax_lien_count = rec.tax_lien_count;
    out.tax_sale_count = rec.tax_sale_count;
    out.foreclosure_filing_count = rec.foreclosure_filing_count;
    out.open_receivership_count = rec.open_receivership_count;
    out.auction_count = rec.auction_count;
    out.tax_lien_amount = rec.tax_lien_amount;
    out.tax_sale_amount = rec.tax_sale_amount;
    out.current_value = rec.current_value;
    out.latest_foreclosure_date = rec.latest_foreclosure_date;
    out.next_auction_date = rec.next_auction_date;
    out.last_sold_at_auction_date = rec.last_sold_at_auction_date;
    return out;
}

std::string coordinatesForExtent(const LayerDef::FeatureExtent& extent) {
    if (extent.min_lon == 0.0f && extent.max_lon == 0.0f && extent.min_lat == 0.0f && extent.max_lat == 0.0f) {
        return {};
    }
    const double lon = ((double)extent.min_lon + (double)extent.max_lon) * 0.5;
    const double lat = ((double)extent.min_lat + (double)extent.max_lat) * 0.5;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.7f,%.7f", lat, lon);
    return buf;
}

void drawLookupButton(const char* label, const std::string& url) {
    ImGui::BeginDisabled(url.empty());
    if (ImGui::Button(label) && !url.empty()) openUrlInBrowser(url);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !url.empty()) {
        ImGui::SetTooltip("%s", url.c_str());
    }
}

void drawParcelExternalActivityLookup(
    const UnifiedParcelRecord* selected_unified,
    const DuckDbParcelDetailSnapshot& duckdb_detail,
    const LayerDef::FeatureRecord* selected_rp,
    const std::string& fallback_blocklot) {
    std::string address = selected_unified ? trimDisplayValue(selected_unified->address) : std::string();
    std::string owner = selected_unified
        ? trimDisplayValue(selected_unified->owner_display.empty() ? selected_unified->owner : selected_unified->owner_display)
        : std::string();
    LayerDef::FeatureExtent extent = selected_unified ? selected_unified->parcel_extent : LayerDef::FeatureExtent{};
    if (address.empty() && duckdb_detail.ok) address = trimDisplayValue(duckdb_detail.address);
    if (owner.empty() && duckdb_detail.ok) owner = trimDisplayValue(duckdb_detail.owner_display);
    if (extent.min_lon == 0.0f && extent.max_lon == 0.0f && extent.min_lat == 0.0f && extent.max_lat == 0.0f && duckdb_detail.ok) {
        extent = duckdb_detail.parcel_extent;
    }
    if (address.empty() && selected_rp) {
        address = trimDisplayValue(firstDisplayProperty(
            *selected_rp,
            {"address", "property_address", "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
             "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1", "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"}));
    }
    if (owner.empty() && selected_rp) {
        owner = trimDisplayValue(firstDisplayProperty(
            *selected_rp,
            {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"}));
    }
    if (extent.min_lon == 0.0f && extent.max_lon == 0.0f && extent.min_lat == 0.0f && extent.max_lat == 0.0f && selected_rp) {
        extent = selected_rp->extent;
    }
    const std::string coords = coordinatesForExtent(extent);
    const std::string location_query = !address.empty() ? address : coords;
    const std::string search_subject =
        trimDisplayValue((!address.empty() ? address : coords) + std::string(owner.empty() ? "" : " " + owner));
    const std::string activity_query =
        trimDisplayValue((!address.empty() ? address : coords) + std::string(" commercial industrial business occupants"));
    const std::string blocklot_query = trimDisplayValue(fallback_blocklot.empty() ? std::string() : fallback_blocklot + " Baltimore parcel");

    const std::string maps_url = location_query.empty()
        ? std::string()
        : "https://www.google.com/maps/search/?api=1&query=" + urlEncodeComponent(location_query);
    const std::string business_maps_url = activity_query.empty()
        ? std::string()
        : "https://www.google.com/maps/search/?api=1&query=" + urlEncodeComponent(activity_query);
    const std::string search_url = search_subject.empty()
        ? std::string()
        : "https://www.google.com/search?q=" + urlEncodeComponent(search_subject);
    const std::string osm_url = location_query.empty()
        ? std::string()
        : "https://www.openstreetmap.org/search?query=" + urlEncodeComponent(location_query);
    const std::string parcel_search_url = blocklot_query.empty()
        ? std::string()
        : "https://www.google.com/search?q=" + urlEncodeComponent(blocklot_query);

    ImGui::SeparatorText("External Activity Lookup");
    if (!address.empty()) ImGui::TextWrapped("Lookup address: %s", address.c_str());
    else if (!coords.empty()) ImGui::TextWrapped("Lookup centroid: %s", coords.c_str());
    if (!owner.empty()) ImGui::TextWrapped("Owner/name clue: %s", owner.c_str());
    ImGui::TextDisabled("External links open live sources; results are not scraped or stored.");
    drawLookupButton("Google Maps", maps_url);
    ImGui::SameLine();
    drawLookupButton("Business/Activity Search", business_maps_url);
    drawLookupButton("Google Search", search_url);
    ImGui::SameLine();
    drawLookupButton("Parcel Search", parcel_search_url);
    ImGui::SameLine();
    drawLookupButton("OpenStreetMap", osm_url);
}

bool drawDuckDbParcelDetail(OwnerInfoUiState* owner_info_state, DuckDbAnalytics* duckdb_analytics, const std::string& parcel_entity_id) {
    if (!duckdb_analytics || !duckdb_analytics->ensureReady()) return false;
    const DuckDbUnifiedParcelDetail detail = duckdb_analytics->queryUnifiedParcelDetailRecord(parcel_entity_id);
    if (!detail.ok || !detail.found) return false;
    const UnifiedParcelRecord& rec = detail.record;
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
    ImGui::Text("Foreclosure Filings: %d", rec.foreclosure_filing_count);
    ImGui::Text("Open Receiverships: %d", rec.open_receivership_count);
    ImGui::Text("Auction Dates Known: %d", rec.auction_count);
    numeric_prop("Tax Lien Amount", rec.tax_lien_amount);
    numeric_prop("Tax Sale Amount", rec.tax_sale_amount);
    text_prop("Latest Foreclosure Filing", rec.latest_foreclosure_date);
    text_prop("Next Auction", rec.next_auction_date);
    text_prop("Last Sold at Auction", rec.last_sold_at_auction_date);
    if (owner_info_state) {
        drawSourceInfoLink(*owner_info_state, "Parcel Source:", rec.parcel_source_file, false, "parcel_info_duckdb_parcel_source");
        drawSourceInfoLink(*owner_info_state, "Property Source:", rec.property_source_file, true, "parcel_info_duckdb_property_source");
    } else {
        text_prop("Parcel Source", rec.parcel_source_file);
        text_prop("Property Source", rec.property_source_file);
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
    ImGui::Text("Foreclosure Filings: %d", rec.foreclosure_filing_count);
    ImGui::Text("Open Receiverships: %d", rec.open_receivership_count);
    ImGui::Text("Auction Dates Known: %d", rec.auction_count);
    numeric_prop("Tax Lien Amount", rec.tax_lien_amount);
    numeric_prop("Tax Sale Amount", rec.tax_sale_amount);
    text_prop("Latest Foreclosure Filing", rec.latest_foreclosure_date);
    text_prop("Next Auction", rec.next_auction_date);
    text_prop("Last Sold at Auction", rec.last_sold_at_auction_date);
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
            int foreclosure = 0;
            int open_receivership = 0;
            int auction = 0;
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
                    foreclosure += u->foreclosure_filing_count;
                    open_receivership += u->open_receivership_count;
                    auction += u->auction_count;
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
                foreclosure = duckdb_detail.foreclosure_filing_count;
                open_receivership = duckdb_detail.open_receivership_count;
                auction = duckdb_detail.auction_count;
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
            ImGui::Text("Foreclosure Filings: %d", foreclosure);
            ImGui::Text("Open Receiverships: %d", open_receivership);
            ImGui::Text("Auction Dates Known: %d", auction);
            if (ctx.parcel_selection->refs.size() == 1 && duckdb_detail.ok) {
                if (!duckdb_detail.latest_foreclosure_date.empty()) {
                    ImGui::TextWrapped("Latest Foreclosure Filing: %s", duckdb_detail.latest_foreclosure_date.c_str());
                }
                if (!duckdb_detail.next_auction_date.empty()) {
                    ImGui::TextWrapped("Next Auction: %s", duckdb_detail.next_auction_date.c_str());
                }
                if (!duckdb_detail.last_sold_at_auction_date.empty()) {
                    ImGui::TextWrapped("Last Sold at Auction: %s", duckdb_detail.last_sold_at_auction_date.c_str());
                }
            }
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
            drawParcelExternalActivityLookup(
                selected_unified,
                duckdb_detail,
                selected_rp,
                blocklot_raw);
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
