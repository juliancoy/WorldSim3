#include "owner_info.h"

#include "app_utils.h"
#include "feature_props.h"
#include "geo.h"
#include "imgui.h"
#include "parcel_value_ui.h"
#include "parcel_timeline.h"
#include "real_property_ui.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace {
using json = nlohmann::json;
constexpr ImVec4 kDarkModeLinkBlue = ImVec4(0.42f, 0.72f, 1.00f, 1.0f);
constexpr ImVec4 kDarkModeLinkBlueBg = ImVec4(0.18f, 0.38f, 0.72f, 0.10f);
constexpr ImVec4 kDarkModeLinkBlueHoverBg = ImVec4(0.20f, 0.40f, 0.72f, 0.22f);
constexpr ImVec4 kDarkModeLinkBlueActiveBg = ImVec4(0.16f, 0.34f, 0.62f, 0.32f);

ElementInfoEntry currentEntry(const ElementInfoUiState& state) {
    if (state.history_index >= state.history.size()) return {};
    return state.history[state.history_index];
}

bool sameEntry(const ElementInfoEntry& a, const ElementInfoEntry& b) {
    return a.kind == b.kind &&
           a.parcel_entity_id == b.parcel_entity_id &&
           a.owner == b.owner &&
           a.source == b.source;
}

void clearOwnerPropertyQuery(ElementInfoUiState& state) {
    if (state.property_query && state.property_query_size > 0) state.property_query[0] = '\0';
}

std::string ownerLabelForRecord(const UnifiedParcelRecord& parcel) {
    return trimDisplayValue(parcel.owner_display.empty() ? parcel.owner : parcel.owner_display);
}

std::string ownerLookupKey(const std::string& owner) {
    return canonicalOwnerName(trimDisplayValue(owner));
}

std::string ownerLookupKeyForRecord(const UnifiedParcelRecord& parcel) {
    if (!parcel.owner.empty()) return parcel.owner;
    return ownerLookupKey(ownerLabelForRecord(parcel));
}

bool parcelMatchesOwner(const UnifiedParcelRecord& parcel, const std::string& owner) {
    const std::string owner_key = ownerLookupKey(owner);
    return !owner_key.empty() && ownerLookupKeyForRecord(parcel) == owner_key;
}

std::vector<OwnerSimilarMatch> buildSimilarOwnerMatches(
    const std::vector<UnifiedParcelRecord>& unified_parcels,
    const std::string& owner,
    int min_score,
    int limit) {
    struct SimilarOwnerAccumulator {
        OwnerSimilarMatch match;
    };

    const std::string owner_label = trimDisplayValue(owner);
    const std::string owner_key = ownerLookupKey(owner_label);
    std::unordered_map<std::string, SimilarOwnerAccumulator> matches_by_key;
    matches_by_key.reserve(256);
    for (const auto& parcel : unified_parcels) {
        const std::string candidate_owner = ownerLabelForRecord(parcel);
        const std::string candidate_key = ownerLookupKeyForRecord(parcel);
        if (candidate_owner.empty() || candidate_key.empty() || candidate_key == owner_key) continue;
        const int score = fuzzyTextScore(candidate_owner, owner_label);
        if (score < min_score) continue;
        auto [it, inserted] = matches_by_key.try_emplace(candidate_key);
        SimilarOwnerAccumulator& acc = it->second;
        if (inserted || acc.match.owner.empty() || candidate_owner.size() < acc.match.owner.size()) {
            acc.match.owner = candidate_owner;
            acc.match.score = score;
        } else if (score > acc.match.score) {
            acc.match.score = score;
        }
        acc.match.property_count += 1;
        acc.match.current_value += parcel.current_value;
    }

    std::vector<OwnerSimilarMatch> matches;
    matches.reserve(matches_by_key.size());
    for (auto& kv : matches_by_key) matches.push_back(std::move(kv.second.match));
    std::stable_sort(matches.begin(), matches.end(), [](const OwnerSimilarMatch& a, const OwnerSimilarMatch& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.property_count != b.property_count) return a.property_count > b.property_count;
        if (a.current_value != b.current_value) return a.current_value > b.current_value;
        return a.owner < b.owner;
    });
    if (limit > 0 && (size_t)limit < matches.size()) matches.resize((size_t)limit);
    return matches;
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

struct GeoBounds {
    bool valid = false;
    double min_lon = 0.0;
    double min_lat = 0.0;
    double max_lon = 0.0;
    double max_lat = 0.0;
};

void expandBounds(GeoBounds& bounds, const LayerDef::FeatureExtent& extent) {
    if (!bounds.valid) {
        bounds.valid = true;
        bounds.min_lon = extent.min_lon;
        bounds.min_lat = extent.min_lat;
        bounds.max_lon = extent.max_lon;
        bounds.max_lat = extent.max_lat;
        return;
    }
    bounds.min_lon = std::min(bounds.min_lon, (double)extent.min_lon);
    bounds.min_lat = std::min(bounds.min_lat, (double)extent.min_lat);
    bounds.max_lon = std::max(bounds.max_lon, (double)extent.max_lon);
    bounds.max_lat = std::max(bounds.max_lat, (double)extent.max_lat);
}

int fitZoomForBounds(const GeoBounds& bounds, float map_view_w, float map_view_h, int min_zoom, int max_zoom) {
    if (!bounds.valid) return std::clamp(18, min_zoom, max_zoom);
    const float pad_px = 96.0f;
    const double avail_w = std::max(64.0, (double)map_view_w - pad_px);
    const double avail_h = std::max(64.0, (double)map_view_h - pad_px);
    for (int z = max_zoom; z >= min_zoom; --z) {
        const ImVec2 nw = lonLatToWorldPx(bounds.min_lon, bounds.max_lat, z);
        const ImVec2 se = lonLatToWorldPx(bounds.max_lon, bounds.min_lat, z);
        const double span_w = std::fabs((double)se.x - (double)nw.x);
        const double span_h = std::fabs((double)se.y - (double)nw.y);
        if (span_w <= avail_w && span_h <= avail_h) return z;
    }
    return min_zoom;
}

void applyBoundsView(
    const OwnerInfoTabContext& ctx,
    const GeoBounds& bounds) {
    if (!bounds.valid || !ctx.center_lon || !ctx.center_lat || !ctx.zoom) return;
    *ctx.center_lon = (bounds.min_lon + bounds.max_lon) * 0.5;
    *ctx.center_lat = std::clamp((bounds.min_lat + bounds.max_lat) * 0.5, -85.0, 85.0);
    *ctx.zoom = fitZoomForBounds(bounds, ctx.map_view_w, ctx.map_view_h, ctx.min_zoom, ctx.max_zoom);
}

struct DuckDbParcelDetailSnapshot {
    bool ok = false;
    std::string parcel_entity_id;
    std::string blocklot;
    std::string owner;
    std::string owner_display;
    std::string address;
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
    out.parcel_entity_id = cell("parcel_entity_id");
    out.blocklot = cell("blocklot");
    out.owner = cell("owner");
    out.owner_display = cell("owner_display");
    out.address = cell("address");
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
    ImGui::PushStyleColor(ImGuiCol_Button, kDarkModeLinkBlueBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kDarkModeLinkBlueHoverBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kDarkModeLinkBlueActiveBg);
    ImGui::PushStyleColor(ImGuiCol_Text, kDarkModeLinkBlue);
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
        ImGui::ColorConvertFloat4ToU32(kDarkModeLinkBlue),
        1.0f);
    ImGui::PopStyleColor(4);
    ImGui::PopID();
}

bool drawDuckDbParcelDetail(ElementInfoUiState* state, DuckDbAnalytics* duckdb_analytics, const std::string& parcel_entity_id) {
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
    text_prop("Structure Area (sq ft)", "structure_area_sqft");
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
    if (rec.structure_area_sqft > 0.0) {
        ImGui::TextWrapped("Structure Area (sq ft): %s", formatUsNumber(rec.structure_area_sqft, 0).c_str());
    }
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
    for (size_t row_idx = 0; row_idx < result.rows.size(); ++row_idx) {
        const auto& row = result.rows[row_idx];
        const std::string date = trimDisplayValue(cell(row, "event_date"));
        const std::string year = trimDisplayValue(cell(row, "event_year"));
        const std::string event_type = trimDisplayValue(cell(row, "event_type"));
        const std::string event_label = trimDisplayValue(cell(row, "event_label"));
        const std::string event_title = trimDisplayValue(cell(row, "event_title"));
        const std::string event_detail = trimDisplayValue(cell(row, "event_detail"));
        const std::string event_metadata_json = trimDisplayValue(cell(row, "event_metadata_json"));
        const std::string status = trimDisplayValue(cell(row, "event_status"));
        const std::string amount = trimDisplayValue(cell(row, "amount_usd"));
        const std::string source_name = trimDisplayValue(cell(row, "source_layer_name"));
        const std::string source_file = trimDisplayValue(cell(row, "source_layer_file"));
        const std::string date_label =
            !date.empty() && date != "NULL" ? date : (!year.empty() && year != "NULL" ? year : "(date unavailable)");
        const std::string primary_label =
            !event_label.empty() && event_label != "NULL"
                ? event_label
                : (event_type.empty() || event_type == "NULL" ? "Event" : event_type);
        ImGui::TextWrapped("%s - %s", date_label.c_str(), primary_label.c_str());
        if (!event_title.empty() && event_title != "NULL" && event_title != primary_label) {
            ImGui::TextWrapped("%s", event_title.c_str());
        }
        if (!event_detail.empty() && event_detail != "NULL" && event_detail != event_title) {
            ImGui::TextWrapped("%s", event_detail.c_str());
        }
        if (!status.empty() && status != "NULL") ImGui::TextWrapped("Status: %s", status.c_str());
        if (!amount.empty() && amount != "NULL") {
            const double amount_value = parseNumericField(amount);
            if (amount_value > 0.0) ImGui::TextWrapped("Amount: %s", formatUsd(amount_value, 2).c_str());
            else ImGui::TextWrapped("Amount: %s", amount.c_str());
        }
        if (!event_metadata_json.empty() && event_metadata_json != "NULL") {
            const json metadata = json::parse(event_metadata_json, nullptr, false);
            ImGui::PushID((int)row_idx);
            if (metadata.is_object() && !metadata.empty() && ImGui::TreeNode("Additional Fields")) {
                for (auto it = metadata.begin(); it != metadata.end(); ++it) {
                    if (it.value().is_string()) {
                        ImGui::TextWrapped("%s: %s", it.key().c_str(), it.value().get_ref<const std::string&>().c_str());
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (!source_name.empty() && source_name != "NULL") ImGui::TextDisabled("Source: %s", source_name.c_str());
        else if (!source_file.empty() && source_file != "NULL") ImGui::TextDisabled("Source: %s", source_file.c_str());
        ImGui::Separator();
    }
    ImGui::EndChild();
    return true;
}

size_t duckDbParcelTimelineCount(DuckDbAnalytics* duckdb_analytics, const std::string& blocklot) {
    if (!duckdb_analytics || !duckdb_analytics->status().last_rebuild_ok || trimDisplayValue(blocklot).empty()) return 0;
    const DuckDbQueryResult result = duckdb_analytics->queryParcelEvents(blocklot, 256);
    if (!result.ok) return 0;
    return result.rows.size();
}

bool drawLocalParcelTimeline(
    const std::vector<LayerDef>* layers,
    const std::string& parcel_blocklot,
    const LayerDef::FeatureExtent* parcel_extent,
    const LayerDef::FeatureRecord* real_property,
    int vacant_notice_layer_idx,
    int vacant_rehab_layer_idx,
    int tax_lien_layer_idx,
    int tax_sale_layer_idx) {
    const std::vector<ParcelTimelineEvent> events = buildParcelTimeline(ParcelTimelineRequest{
        layers,
        parcel_blocklot,
        parcel_extent != nullptr,
        parcel_extent ? *parcel_extent : LayerDef::FeatureExtent{},
        real_property,
        vacant_notice_layer_idx,
        vacant_rehab_layer_idx,
        tax_lien_layer_idx,
        tax_sale_layer_idx
    });
    if (events.empty()) return false;

    ImGui::TextDisabled("%zu event(s), newest first", events.size());
    ImGui::BeginChild("parcel_history_events", ImVec2(0, 260.0f), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (const ParcelTimelineEvent& event : events) {
        const std::string date_label = event.date.empty() ? "(date unavailable)" : event.date;
        ImGui::TextWrapped(
            "%s - %s",
            date_label.c_str(),
            event.event_type.empty() ? "Event" : event.event_type.c_str());
        if (!event.status.empty()) ImGui::TextWrapped("Status: %s", event.status.c_str());
        if (!event.amount.empty()) {
            const double amount_value = parseNumericField(event.amount);
            if (amount_value > 0.0) ImGui::TextWrapped("Amount: %s", formatUsd(amount_value, 2).c_str());
            else ImGui::TextWrapped("Amount: %s", event.amount.c_str());
        }
        if (!event.source_layer.empty()) ImGui::TextDisabled("Source: %s", event.source_layer.c_str());
        ImGui::Separator();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Timeline source: loaded runtime parcel-related layers");
    return true;
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

void drawParcelElement(const OwnerInfoTabContext& ctx, const std::string& parcel_entity_id) {
    const UnifiedParcelRecord* selected_unified = ctx.unified_parcels
        ? unifiedParcelAt(*ctx.unified_parcels, parcel_entity_id)
        : nullptr;
    const DuckDbParcelDetailSnapshot duckdb_detail =
        loadDuckDbParcelDetailSnapshot(ctx.duckdb_analytics, parcel_entity_id);
    const bool parcel_info_valid = selected_unified != nullptr || duckdb_detail.ok;

    if (!parcel_info_valid) {
        ImGui::TextDisabled("Parcel is unavailable.");
        return;
    }

    if (ImGui::Button("Clear Parcel Selection")) {
        if (ctx.clear_parcel_selection) ctx.clear_parcel_selection();
    }

    std::string blocklot_raw = selected_unified ? selected_unified->blocklot : std::string();
    int vac_notice = 0;
    int vac_rehab = 0;
    int tax_lien = 0;
    int tax_sale = 0;
    double tax_lien_amount = 0.0;
    double tax_sale_amount = 0.0;
    double current_value_total = 0.0;
    if (ctx.unified_parcels) {
        if (ctx.show_selected_parcel_details && ctx.selected_parcel_ids && !ctx.selected_parcel_ids->empty()) {
            for (const std::string& selected_id : *ctx.selected_parcel_ids) {
                const UnifiedParcelRecord* u = unifiedParcelAt(*ctx.unified_parcels, selected_id);
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
    if (!(ctx.show_selected_parcel_details && ctx.selected_parcel_ids && ctx.selected_parcel_ids->size() > 1) &&
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
    const size_t selected_count = (ctx.show_selected_parcel_details && ctx.selected_parcel_ids)
        ? ctx.selected_parcel_ids->size()
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

    std::string summary_owner = selected_unified ? ownerLabelForRecord(*selected_unified) : duckdb_detail.owner;
    if (summary_owner.empty()) summary_owner = duckdb_detail.owner_display;
    if (summary_owner.empty()) summary_owner = normalizedRealPropertyOwnerName(selected_rp);
    if (!summary_owner.empty() && ctx.state) {
        ImGui::TextUnformatted("Owner:");
        ImGui::SameLine();
        ImGui::PushID("open_owner_info_element_tab");
        ImGui::PushStyleColor(ImGuiCol_Button, kDarkModeLinkBlueBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kDarkModeLinkBlueHoverBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kDarkModeLinkBlueActiveBg);
        ImGui::PushStyleColor(ImGuiCol_Text, kDarkModeLinkBlue);
        if (ImGui::Button(summary_owner.c_str())) {
            openOwnerInfoPageAndSelectOwnerParcels(
                *ctx.state,
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
            ImGui::ColorConvertFloat4ToU32(kDarkModeLinkBlue),
            1.0f);
        ImGui::PopStyleColor(4);
        ImGui::PopID();
    }
    const std::string timeline_blocklot = duckdb_detail.ok ? duckdb_detail.blocklot : blocklot_raw;
    const size_t duckdb_timeline_event_count = duckDbParcelTimelineCount(ctx.duckdb_analytics, timeline_blocklot);
    if (duckdb_timeline_event_count > 0) {
        ImGui::TextColored(ImVec4(0.98f, 0.78f, 0.22f, 1.0f), "History Available: %zu event(s)", duckdb_timeline_event_count);
    }

    if (duckdb_detail.ok) {
        drawDuckDbParcelDetail(ctx.state, ctx.duckdb_analytics, parcel_entity_id);
    } else if (selected_unified) {
        drawUnifiedParcelDetail(ctx.state, *selected_unified);
    } else if (!drawDuckDbParcelDetail(ctx.state, ctx.duckdb_analytics, parcel_entity_id)) {
        drawRealPropertySummary(selected_rp, false);
    }

    const std::string history_title =
        duckdb_timeline_event_count > 0
            ? ("Parcel History (" + std::to_string(duckdb_timeline_event_count) + " Events)")
            : "Parcel History";
    ImGui::SeparatorText(history_title.c_str());
    const bool duckdb_timeline_available =
        ctx.duckdb_analytics &&
        ctx.duckdb_analytics->status().last_rebuild_ok &&
        !trimDisplayValue(timeline_blocklot).empty();
    if (!drawDuckDbParcelTimeline(ctx.duckdb_analytics, timeline_blocklot)) {
        if (!duckdb_timeline_available) {
            ImGui::TextDisabled("DuckDB parcel timeline unavailable; showing runtime-derived timeline.");
            LayerDef::FeatureExtent local_timeline_extent = {};
            const LayerDef::FeatureExtent* local_timeline_extent_ptr = nullptr;
            if (selected_unified && selected_unified->parcel_has_geometry) {
                local_timeline_extent = selected_unified->parcel_extent;
                local_timeline_extent_ptr = &local_timeline_extent;
            }
            if (!drawLocalParcelTimeline(
                    ctx.layers,
                    timeline_blocklot,
                    local_timeline_extent_ptr,
                    selected_rp,
                    ctx.vacant_notice_layer_idx,
                    ctx.vacant_rehab_layer_idx,
                    ctx.tax_lien_layer_idx,
                    ctx.tax_sale_layer_idx)) {
                ImGui::TextDisabled("No parcel history events found in loaded runtime layers.");
            }
        } else {
            ImGui::TextDisabled("No parcel history events found in DuckDB.");
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
        const std::string owner_key = trimDisplayValue(owner);
        std::string owner_label = owner_key;
        std::vector<std::string> owner_parcel_ids;
        owner_parcel_ids.reserve(512);
        double owner_value_total = 0.0;
        for (const auto& parcel_record : *ctx.unified_parcels) {
            if (!parcelMatchesOwner(parcel_record, owner_key)) continue;
            if (owner_label == owner_key) {
                const std::string parcel_owner_label = ownerLabelForRecord(parcel_record);
                if (!parcel_owner_label.empty()) owner_label = parcel_owner_label;
            }
            owner_parcel_ids.push_back(parcel_record.parcel_entity_id);
            owner_value_total += parcel_record.current_value;
        }

        GeoBounds owner_bounds;
        for (const std::string& parcel_id : owner_parcel_ids) {
            const UnifiedParcelRecord* parcel_record = unifiedParcelAt(*ctx.unified_parcels, parcel_id);
            if (!parcel_record || !parcel_record->parcel_has_geometry) continue;
            expandBounds(owner_bounds, parcel_record->parcel_extent);
        }

        ImGui::Text("Element: Owner");
        ImGui::TextWrapped("Owner: %s", owner_label.c_str());
        ImGui::Text("Properties: %zu", owner_parcel_ids.size());
        ImGui::Text("Total Current Value: %s", formatUsd(owner_value_total).c_str());
        if (owner_bounds.valid && ImGui::Button("Zoom To Owner Extent")) {
            applyBoundsView(ctx, owner_bounds);
        }
        if (ctx.state) {
            ImGui::Separator();
            ImGui::TextUnformatted("Similar Owner Names");
            int min_score = std::clamp(ctx.state->owner_fuzzy_match_min_score, 25, 120);
            if (ImGui::SliderInt("Fuzzy Threshold", &min_score, 25, 120)) {
                ctx.state->owner_fuzzy_match_min_score = min_score;
            }
            int match_limit = std::clamp(ctx.state->owner_fuzzy_match_limit, 1, 32);
            if (ImGui::SliderInt("Match Limit", &match_limit, 1, 32)) {
                ctx.state->owner_fuzzy_match_limit = match_limit;
            }
            const std::string owner_cache_key = ownerLookupKey(owner_label);
            if (ctx.state->owner_fuzzy_cache_key != owner_cache_key ||
                ctx.state->owner_fuzzy_cache_min_score != ctx.state->owner_fuzzy_match_min_score ||
                ctx.state->owner_fuzzy_cache_limit != ctx.state->owner_fuzzy_match_limit) {
                ctx.state->owner_fuzzy_cache_key = owner_cache_key;
                ctx.state->owner_fuzzy_cache_min_score = ctx.state->owner_fuzzy_match_min_score;
                ctx.state->owner_fuzzy_cache_limit = ctx.state->owner_fuzzy_match_limit;
                ctx.state->owner_fuzzy_cache_matches = buildSimilarOwnerMatches(
                    *ctx.unified_parcels,
                    owner_label,
                    ctx.state->owner_fuzzy_match_min_score,
                    ctx.state->owner_fuzzy_match_limit);
            }
            const std::vector<OwnerSimilarMatch>& similar_matches = ctx.state->owner_fuzzy_cache_matches;
            if (similar_matches.empty()) {
                ImGui::TextDisabled("No similar owner names at the current threshold.");
            } else {
                ImGui::TextDisabled(
                    "%zu similar owner name%s",
                    similar_matches.size(),
                    similar_matches.size() == 1 ? "" : "s");
                for (size_t i = 0; i < similar_matches.size(); ++i) {
                    const OwnerSimilarMatch& match = similar_matches[i];
                    ImGui::PushID((int)i + 4000);
                    if (ImGui::SmallButton("Open")) {
                        openOwnerInfoPageAndSelectOwnerParcels(
                            *ctx.state,
                            match.owner,
                            ctx.unified_parcels,
                            ctx.clear_parcel_selection,
                            ctx.select_parcel_id);
                    }
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s", match.owner.c_str());
                    ImGui::TextDisabled(
                        "score: %d | properties: %zu | value: %s",
                        match.score,
                        match.property_count,
                        formatUsd(match.current_value).c_str());
                    ImGui::PopID();
                    ImGui::Separator();
                }
            }
        }

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
        for (const std::string& parcel_id : owner_parcel_ids) {
            const UnifiedParcelRecord* parcel_record = unifiedParcelAt(*ctx.unified_parcels, parcel_id);
            if (!parcel_record) continue;
            std::string blocklot = parcel_record->blocklot;
            std::string address = parcel_record->address;
            if (address.empty()) address = "(address unavailable)";
            if (!property_query.empty() &&
                !containsCaseInsensitive(address, property_query) &&
                !containsCaseInsensitive(blocklot, property_query)) {
                continue;
            }

            visible_owner_properties++;
            std::string label = address + "##owner_prop_" + parcel_record->parcel_entity_id;
            const bool row_selected = ctx.selected_parcel_id_set &&
                ctx.selected_parcel_id_set->find(parcel_record->parcel_entity_id) != ctx.selected_parcel_id_set->end();
            if (ImGui::Selectable(label.c_str(), row_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                if (parcel_record->parcel_has_geometry && ctx.center_lon) {
                    *ctx.center_lon = ((double)parcel_record->parcel_extent.min_lon + (double)parcel_record->parcel_extent.max_lon) * 0.5;
                }
                if (parcel_record->parcel_has_geometry && ctx.center_lat) {
                    *ctx.center_lat = std::clamp(
                        ((double)parcel_record->parcel_extent.min_lat + (double)parcel_record->parcel_extent.max_lat) * 0.5,
                        -85.0,
                        85.0);
                }
                if (parcel_record->parcel_has_geometry && ctx.zoom) *ctx.zoom = std::max(*ctx.zoom, 18.0);
                if (ctx.select_parcel_id) ctx.select_parcel_id(parcel_record->parcel_entity_id, ImGui::GetIO().KeyCtrl);
                if (ctx.state) openElementParcelPage(*ctx.state, parcel_record->parcel_entity_id);
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

    std::vector<std::string> source_parcel_ids;
    source_parcel_ids.reserve(1024);
    double source_value_total = 0.0;
    for (const auto& parcel_record : *ctx.unified_parcels) {
        const std::string& candidate = property_source ? parcel_record.property_source_file : parcel_record.parcel_source_file;
        if (candidate != source) continue;
        source_parcel_ids.push_back(parcel_record.parcel_entity_id);
        source_value_total += parcel_record.current_value;
    }

    GeoBounds source_bounds;
    for (const std::string& parcel_id : source_parcel_ids) {
        const UnifiedParcelRecord* parcel_record = unifiedParcelAt(*ctx.unified_parcels, parcel_id);
        if (!parcel_record || !parcel_record->parcel_has_geometry) continue;
        expandBounds(source_bounds, parcel_record->parcel_extent);
    }

    ImGui::Text("Element: %s", property_source ? "Property Source" : "Parcel Source");
    ImGui::TextWrapped("Source: %s", source.c_str());
    ImGui::Text("Properties: %zu", source_parcel_ids.size());
    ImGui::Text("Total Current Value: %s", formatUsd(source_value_total).c_str());
    if (source_bounds.valid && ImGui::Button(property_source ? "Zoom To Property Source Extent" : "Zoom To Parcel Source Extent")) {
        applyBoundsView(ctx, source_bounds);
    }

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
    for (const std::string& parcel_id : source_parcel_ids) {
        const UnifiedParcelRecord* parcel_record = unifiedParcelAt(*ctx.unified_parcels, parcel_id);
        if (!parcel_record) continue;
        std::string blocklot = parcel_record->blocklot;
        std::string address = parcel_record->address;
        if (address.empty()) address = "(address unavailable)";
        if (!property_query.empty() &&
            !containsCaseInsensitive(address, property_query) &&
            !containsCaseInsensitive(blocklot, property_query)) {
            continue;
        }

        visible_source_properties++;
        std::string label = address + "##source_prop_" + parcel_record->parcel_entity_id;
        const bool row_selected = ctx.selected_parcel_id_set &&
            ctx.selected_parcel_id_set->find(parcel_record->parcel_entity_id) != ctx.selected_parcel_id_set->end();
        if (ImGui::Selectable(label.c_str(), row_selected, ImGuiSelectableFlags_SpanAllColumns)) {
            if (parcel_record->parcel_has_geometry && ctx.center_lon) {
                *ctx.center_lon = ((double)parcel_record->parcel_extent.min_lon + (double)parcel_record->parcel_extent.max_lon) * 0.5;
            }
            if (parcel_record->parcel_has_geometry && ctx.center_lat) {
                *ctx.center_lat = std::clamp(
                    ((double)parcel_record->parcel_extent.min_lat + (double)parcel_record->parcel_extent.max_lat) * 0.5,
                    -85.0,
                    85.0);
            }
            if (parcel_record->parcel_has_geometry && ctx.zoom) *ctx.zoom = std::max(*ctx.zoom, 18.0);
            if (ctx.select_parcel_id) ctx.select_parcel_id(parcel_record->parcel_entity_id, ImGui::GetIO().KeyCtrl);
            if (ctx.state) openElementParcelPage(*ctx.state, parcel_record->parcel_entity_id);
        }
        ImGui::TextDisabled("BLOCKLOT: %s", blocklot.empty() ? "(none)" : blocklot.c_str());
        if (parcel_record->current_value > 0.0) ImGui::TextDisabled("Current value: %s", formatUsd(parcel_record->current_value).c_str());
        ImGui::Separator();
    }
    if (visible_source_properties == 0) ImGui::TextDisabled("No matching properties.");
    ImGui::EndChild();
}
}

void openElementParcelPage(ElementInfoUiState& state, const std::string& parcel_entity_id) {
    openElementPage(state, ElementInfoEntry{ElementInfoKind::Parcel, parcel_entity_id, {}, {}});
}

void openOwnerInfoPage(ElementInfoUiState& state, const std::string& owner) {
    openElementPage(state, ElementInfoEntry{ElementInfoKind::Owner, {}, owner, {}});
}

void openOwnerInfoPageAndSelectOwnerParcels(
    ElementInfoUiState& state,
    const std::string& owner,
    const std::vector<UnifiedParcelRecord>* unified_parcels,
    const std::function<void()>& clear_parcel_selection,
    const std::function<bool(const std::string&, bool)>& select_parcel_id) {
    const std::string normalized_owner = trimDisplayValue(owner);
    std::string page_owner = normalized_owner;
    if (!normalized_owner.empty() && unified_parcels && clear_parcel_selection && select_parcel_id) {
        clear_parcel_selection();
        for (const UnifiedParcelRecord& parcel : *unified_parcels) {
            if (!parcelMatchesOwner(parcel, normalized_owner)) continue;
            if (page_owner == normalized_owner) {
                const std::string parcel_owner_label = ownerLabelForRecord(parcel);
                if (!parcel_owner_label.empty()) page_owner = parcel_owner_label;
            }
            select_parcel_id(parcel.parcel_entity_id, true);
        }
    }
    openOwnerInfoPage(state, page_owner.empty() ? owner : page_owner);
}

void openParcelSourceInfoPage(ElementInfoUiState& state, const std::string& source) {
    openElementPage(state, ElementInfoEntry{ElementInfoKind::ParcelSource, {}, {}, source});
}

void openPropertySourceInfoPage(ElementInfoUiState& state, const std::string& source) {
    openElementPage(state, ElementInfoEntry{ElementInfoKind::PropertySource, {}, {}, source});
}

void drawOwnerInfoLink(ElementInfoUiState& state, const std::string& owner, const char* id) {
    ImGui::TextUnformatted("Owner:");
    ImGui::SameLine();
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, kDarkModeLinkBlueBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kDarkModeLinkBlueHoverBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kDarkModeLinkBlueActiveBg);
    ImGui::PushStyleColor(ImGuiCol_Text, kDarkModeLinkBlue);
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
        ImGui::ColorConvertFloat4ToU32(kDarkModeLinkBlue),
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
        drawParcelElement(ctx, entry.parcel_entity_id);
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
