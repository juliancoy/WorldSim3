#include "filters_tab.h"

#include "app_utils.h"
#include "cache_io.h"
#include "feature_props.h"
#include "imgui.h"
#include "layer_geometry.h"
#include "layer_state_io.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <sstream>

namespace {
struct SelectedZoneDetails {
    std::string zone_key;
    std::string zone_label;
    std::string zone_description;
    std::string source_url;
    std::vector<std::pair<std::string, std::string>> fields;
};

struct SelectedZoneDetailsCache {
    int layer_idx = -1;
    size_t feature_idx = (size_t)-1;
    std::string source_signature;
    bool valid = false;
    SelectedZoneDetails details;
};

SelectedZoneDetails buildSelectedZoneDetails(
    const LayerDef& layer,
    const LayerDef::FeatureRecord& selected,
    size_t feature_idx,
    const std::unordered_map<std::string, ZoneMetadata>* zoning_metadata) {
    SelectedZoneDetails details;
    details.zone_key = zoningClassKey(selected);
    details.zone_label = zoningClassLabel(selected);
    details.source_url = trimDisplayValue(getPropertyValue(selected, "URL"));
    if (zoning_metadata) {
        auto meta_it = zoning_metadata->find(details.zone_key);
        if (meta_it != zoning_metadata->end()) {
            if (!meta_it->second.label.empty()) {
                details.zone_label = meta_it->second.label;
            }
            if (details.source_url.empty() && !meta_it->second.source_url.empty()) {
                details.source_url = meta_it->second.source_url;
            }
        }
        details.zone_description = zoningDescription(selected, *zoning_metadata);
    }
    auto add_field = [&](const char* label, const std::string& value) {
        const std::string trimmed = trimDisplayValue(value);
        if (!trimmed.empty()) details.fields.push_back({label, trimmed});
    };
    add_field("Layer", layer.name);
    add_field("Feature", std::to_string(feature_idx));
    add_field("District", getPropertyValue(selected, "ZONE_DIST"));
    add_field("Class", getPropertyValue(selected, "ZONE_CLASS"));
    add_field("District Code", getPropertyValue(selected, "DIST_CODE"));
    add_field("Acres", getPropertyValue(selected, "ACRES"));
    add_field("Object ID", getPropertyValue(selected, "OBJECTID"));
    add_field("Source", details.source_url);
    return details;
}

std::optional<SelectedZoneDetails> selectedZoneDetailsForPanel(const FiltersTabContext& ctx) {
    if (!ctx.show_selected_zone_details || !*ctx.show_selected_zone_details ||
        !ctx.selected_zone_idx || ctx.zoning_layer_idx < 0 ||
        !ctx.layers || (size_t)ctx.zoning_layer_idx >= ctx.layers->size()) {
        return std::nullopt;
    }
    const LayerDef& zoning_layer = (*ctx.layers)[(size_t)ctx.zoning_layer_idx];
    if (*ctx.selected_zone_idx < zoning_layer.features.size()) {
        return buildSelectedZoneDetails(
            zoning_layer,
            zoning_layer.features[*ctx.selected_zone_idx],
            *ctx.selected_zone_idx,
            ctx.zoning_metadata);
    }
    if (!ctx.root) return std::nullopt;

    std::string source_signature;
    const std::filesystem::path layer_path = resolveStoredLayerPath(*ctx.root, zoning_layer);
    if (!resolveLayerSourceSignature(layer_path, source_signature, nullptr) || source_signature.empty()) {
        return std::nullopt;
    }

    static SelectedZoneDetailsCache fallback_cache;
    if (fallback_cache.valid &&
        fallback_cache.layer_idx == ctx.zoning_layer_idx &&
        fallback_cache.feature_idx == *ctx.selected_zone_idx &&
        fallback_cache.source_signature == source_signature) {
        return fallback_cache.details;
    }

    std::vector<LayerDef::FeatureRecord> features;
    std::vector<LayerDef::FeatureProperties> properties;
    if (!loadCanonicalLayerFeatureCollection(*ctx.root, zoning_layer.file, source_signature, features, &properties) ||
        *ctx.selected_zone_idx >= features.size()) {
        fallback_cache.valid = false;
        return std::nullopt;
    }
    LayerDef::FeatureRecord& selected = features[*ctx.selected_zone_idx];
    if (*ctx.selected_zone_idx < properties.size()) {
        setTransientFeatureProperties(selected, properties[*ctx.selected_zone_idx].values);
    }
    SelectedZoneDetails details = buildSelectedZoneDetails(
        zoning_layer,
        selected,
        *ctx.selected_zone_idx,
        ctx.zoning_metadata);
    clearTransientFeatureProperties(selected);

    fallback_cache.layer_idx = ctx.zoning_layer_idx;
    fallback_cache.feature_idx = *ctx.selected_zone_idx;
    fallback_cache.source_signature = source_signature;
    fallback_cache.valid = true;
    fallback_cache.details = details;
    return details;
}
}

void drawFiltersTab(const FiltersTabContext& ctx) {
    if (!ImGui::BeginTabItem("Filters")) return;

    if (!ctx.filters || !ctx.layers || !ctx.show_selected_parcel_details ||
        !ctx.show_selected_zone_details || !ctx.selected_zone_idx ||
        !ctx.center_lon || !ctx.center_lat || !ctx.zoom ||
        !ctx.address_locate_status || !ctx.address_locate_matches) {
        ImGui::TextDisabled("Filters tab context is incomplete.");
        ImGui::EndTabItem();
        return;
    }

    auto focus_parcel_by_id = [&](const std::string& entity_id) -> bool {
        if (entity_id.empty()) return false;
        if (ctx.parcel_layer_idx < 0 || (size_t)ctx.parcel_layer_idx >= ctx.layers->size()) return false;
        const auto& parcel_layer = (*ctx.layers)[(size_t)ctx.parcel_layer_idx];
        const size_t idx = featureIndexForEntityId(parcel_layer, entity_id);
        if (idx >= parcel_layer.features.size()) return false;
        LayerDef::FeatureExtent parcel_extent = {};
        bool has_parcel_extent = false;
        if (ctx.unified_parcels) {
            if (const UnifiedParcelRecord* rec = unifiedParcelAt(*ctx.unified_parcels, entity_id);
                rec && rec->parcel_has_geometry) {
                parcel_extent = rec->parcel_extent;
                has_parcel_extent = true;
            }
        }
        if (!has_parcel_extent) {
            parcel_extent = parcel_layer.features[idx].extent;
            has_parcel_extent = true;
        }
        if (has_parcel_extent) {
            *ctx.center_lon = ((double)parcel_extent.min_lon + (double)parcel_extent.max_lon) * 0.5;
            *ctx.center_lat = std::clamp(((double)parcel_extent.min_lat + (double)parcel_extent.max_lat) * 0.5, -85.0, 85.0);
            *ctx.zoom = std::max(*ctx.zoom, 18.0);
        }
        return ctx.select_parcel_id ? ctx.select_parcel_id(entity_id, ImGui::GetIO().KeyCtrl) : false;
    };

    auto locate_property_by_address = [&]() {
        const std::string query = trimDisplayValue(ctx.filters->address);
        ctx.address_locate_matches->clear();
        if (query.empty()) {
            *ctx.address_locate_status = "Enter an address to locate a property.";
            return;
        }
        if (ctx.parcel_layer_idx < 0 || (size_t)ctx.parcel_layer_idx >= ctx.layers->size()) {
            *ctx.address_locate_status = "Parcel layer is not available.";
            return;
        }
        const auto& parcel_layer = (*ctx.layers)[(size_t)ctx.parcel_layer_idx];
        if (parcel_layer.features.empty()) {
            *ctx.address_locate_status = "Parcel layer is still loading or missing.";
            return;
        }
        if (ctx.duckdb_analytics && ctx.duckdb_analytics->status().last_rebuild_ok) {
            const std::vector<DuckDbSearchHit> hits = ctx.duckdb_analytics->searchParcels(query, 24);
            for (const auto& hit : hits) {
                if (hit.layer_idx != (size_t)ctx.parcel_layer_idx) continue;
                if (hit.entity_id.empty()) continue;
                ctx.address_locate_matches->push_back({hit.entity_id, hit.score, hit.address.empty() ? hit.blocklot : hit.address});
            }
            if (!ctx.address_locate_matches->empty()) {
                const auto& best = ctx.address_locate_matches->front();
                if (!focus_parcel_by_id(best.parcel_entity_id)) {
                    *ctx.address_locate_status = "DuckDB fuzzy match found, but parcel geometry is unavailable.";
                    return;
                }
                *ctx.address_locate_status = "Found " + std::to_string(ctx.address_locate_matches->size()) + " DuckDB fuzzy matches.";
                return;
            }
        }
        auto property_address_for = [&](const LayerDef::FeatureRecord& parcel) {
            std::string address = firstDisplayProperty(parcel, {
                "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
                "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
                "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
            });
            if (!address.empty()) return address;
            if (ctx.layers) {
                if (const LayerDef::FeatureRecord* rp = resolveRealPropertyForBlocklot(
                        *ctx.layers,
                        ctx.real_property_layer_idx,
                        ctx.real_property_by_blocklot,
                        featureBlockLotJoinKey(parcel))) {
                    address = firstDisplayProperty(*rp, {
                        "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
                        "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
                        "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
                    });
                }
            }
            return address;
        };

        constexpr size_t kMaxAddressMatches = 24;
        for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
            const auto& parcel = parcel_layer.features[i];
            const std::string address = property_address_for(parcel);
            const int score = std::max(addressSearchScore(address, query), fuzzyTextScore(address, query));
            if (score <= 0) continue;
            ctx.address_locate_matches->push_back({
                featureEntityIdForLayerFeature(parcel_layer, parcel, i),
                score,
                address
            });
        }
        if (ctx.address_locate_matches->empty()) {
            *ctx.address_locate_status = "No matching property address found.";
            return;
        }
        std::stable_sort(ctx.address_locate_matches->begin(), ctx.address_locate_matches->end(), [](const AddressLocateMatch& a, const AddressLocateMatch& b) {
            if (a.score != b.score) return a.score > b.score;
            if (a.address.size() != b.address.size()) return a.address.size() < b.address.size();
            return a.parcel_entity_id < b.parcel_entity_id;
        });
        if (ctx.address_locate_matches->size() > kMaxAddressMatches) ctx.address_locate_matches->resize(kMaxAddressMatches);
        const auto& best = ctx.address_locate_matches->front();
        if (!focus_parcel_by_id(best.parcel_entity_id)) {
            *ctx.address_locate_status = "Address match found, but parcel geometry is unavailable.";
            return;
        }
        *ctx.address_locate_status = "Found " + std::to_string(ctx.address_locate_matches->size()) + " matches.";
    };

    const std::optional<SelectedZoneDetails> selected_zone = selectedZoneDetailsForPanel(ctx);
    const bool selected_zone_valid = selected_zone.has_value();

    if (selected_zone_valid) {
        if (ImGui::Button("Back To Filters")) {
            *ctx.show_selected_zone_details = false;
            *ctx.selected_zone_idx = (size_t)-1;
        }
        ImGui::Separator();
        const std::string& zone_key = selected_zone->zone_key;
        const std::string& zone_label = selected_zone->zone_label;
        const std::string& zone_description = selected_zone->zone_description;
        const char* display_zone = !zone_label.empty() ? zone_label.c_str() : (zone_key.empty() ? "(unlabeled)" : zone_key.c_str());
        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextWrapped("%s", display_zone);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Separator();
        ImGui::TextWrapped("%s", zone_description.empty() ? "No description available." : zone_description.c_str());
        if (!selected_zone->fields.empty()) {
            ImGui::SeparatorText("Official Source Fields");
            if (ImGui::BeginTable("selected_zone_fields", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 112.0f);
                ImGui::TableSetupColumn("Value");
                for (const auto& field : selected_zone->fields) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("%s", field.first.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", field.second.c_str());
                }
                ImGui::EndTable();
            }
        }
        if (!selected_zone->source_url.empty()) {
            if (ImGui::Button("Open Official Zoning Sheet")) {
                openUrlInBrowser(selected_zone->source_url);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "Official PDF: %s\n\n%s",
                    selected_zone->source_url.c_str(),
                    zone_description.empty() ? "No extracted summary available." : zone_description.c_str());
            }
        }
        ImGui::EndTabItem();
        return;
    }

    if (*ctx.show_selected_zone_details && !selected_zone_valid) {
        *ctx.show_selected_zone_details = false;
        *ctx.selected_zone_idx = (size_t)-1;
    }

    ImGui::Checkbox("Enable Filters", &ctx.filters->enabled);
    ImGui::Checkbox("Filter By Record Date", &ctx.filters->use_date);
    ImGui::BeginDisabled(!ctx.filters->enabled || !ctx.filters->use_date);
    ImGui::SliderInt("Year Min", &ctx.filters->year_min, 1900, 2100);
    ImGui::SliderInt("Year Max", &ctx.filters->year_max, 1900, 2100);
    if (ctx.filters->year_min > ctx.filters->year_max) std::swap(ctx.filters->year_min, ctx.filters->year_max);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Address Search");
    const float locate_button_w = 78.0f;
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - locate_button_w - ImGui::GetStyle().ItemSpacing.x));
    const bool locate_enter = ImGui::InputTextWithHint(
        "##address_search",
        "Search property address, e.g. 123 N Charles, Charles 123, or 123 north charles street",
        ctx.filters->address,
        sizeof(ctx.filters->address),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Locate", ImVec2(locate_button_w, 0.0f)) || locate_enter) {
        locate_property_by_address();
    }
    if (!ctx.address_locate_status->empty()) ImGui::TextWrapped("%s", ctx.address_locate_status->c_str());
    if (!ctx.address_locate_matches->empty()) {
        ImGui::TextDisabled("Top matches");
        ImGui::BeginChild("address_match_list", ImVec2(0, 120), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        for (size_t i = 0; i < ctx.address_locate_matches->size(); ++i) {
            const auto& m = (*ctx.address_locate_matches)[i];
            std::string label = m.address.empty() ? m.parcel_entity_id : m.address;
            label += "##addr_match_" + std::to_string(i);
            const bool row_selected = ctx.selected_parcel_id_set &&
                ctx.selected_parcel_id_set->find(m.parcel_entity_id) != ctx.selected_parcel_id_set->end();
            if (ImGui::Selectable(label.c_str(), row_selected)) {
                if (focus_parcel_by_id(m.parcel_entity_id)) {
                    *ctx.address_locate_status = "Located: " + (m.address.empty() ? std::string("matching parcel") : m.address);
                }
            }
        }
        ImGui::EndChild();
    }
    if (!ctx.filters->enabled) {
        ImGui::TextDisabled("Locate works anytime. Enable filters to also narrow visible records by address.");
    } else {
        ImGui::TextDisabled("Address search ignores case, punctuation, token order, and common street suffix variants.");
    }

    ImGui::SeparatorText("Other Fields");
    ImGui::InputText("Block/Lot", ctx.filters->blocklot, sizeof(ctx.filters->blocklot));
    ImGui::InputText("Status", ctx.filters->status, sizeof(ctx.filters->status));
    ImGui::InputText("Owner", ctx.filters->owner, sizeof(ctx.filters->owner));
    ImGui::InputText("ZIP", ctx.filters->zip, sizeof(ctx.filters->zip));
    if (ImGui::Button("Clear Field Filters")) {
        ctx.filters->blocklot[0] = '\0';
        ctx.filters->status[0] = '\0';
        ctx.filters->address[0] = '\0';
        ctx.filters->owner[0] = '\0';
        ctx.filters->zip[0] = '\0';
        ctx.filters->use_date = false;
    }

    if (!ctx.record_year_hist || !ctx.record_year_hist_plot || !ctx.hist_feature_counts ||
        !ctx.hist_enabled || !ctx.hist_dirty || !ctx.record_year_hist_max_bin ||
        !ctx.record_year_nonzero_min || !ctx.record_year_nonzero_max ||
        !ctx.record_year_nonzero_total || !ctx.selected_record_year ||
        !ctx.selected_record_year_dirty || !ctx.selected_record_year_total ||
        !ctx.selected_record_year_samples) {
        ImGui::EndTabItem();
        return;
    }

    ImGui::SeparatorText("Record Year Histogram");
    if (ctx.hist_feature_counts->size() != ctx.layers->size()) {
        ctx.hist_feature_counts->assign(ctx.layers->size(), 0);
        ctx.hist_enabled->assign(ctx.layers->size(), false);
        *ctx.hist_dirty = true;
    }
    for (size_t i = 0; i < ctx.layers->size(); ++i) {
        const size_t fc = (*ctx.layers)[i].features.size();
        if ((*ctx.hist_feature_counts)[i] != fc || (*ctx.hist_enabled)[i] != (*ctx.layers)[i].enabled) {
            (*ctx.hist_feature_counts)[i] = fc;
            (*ctx.hist_enabled)[i] = (*ctx.layers)[i].enabled;
            *ctx.hist_dirty = true;
        }
    }
    if (*ctx.hist_dirty) {
        std::fill(ctx.record_year_hist->begin(), ctx.record_year_hist->end(), 0);
        for (size_t li = 0; li < ctx.layers->size(); ++li) {
            if (!(*ctx.layers)[li].enabled) continue;
            for (const auto& fg : (*ctx.layers)[li].features) {
                std::string ds = firstDisplayProperty(fg, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
                if (ds.empty()) continue;
                int y = extractYearMaybe(ds);
                if (y < 1900 || y > 2100) continue;
                (*ctx.record_year_hist)[(size_t)(y - 1900)]++;
            }
        }
        float max_bin = 1.0f;
        int nz_min = 2101;
        int nz_max = 1899;
        int nz_total = 0;
        for (size_t i = 0; i < ctx.record_year_hist->size(); ++i) {
            (*ctx.record_year_hist_plot)[i] = (float)(*ctx.record_year_hist)[i];
            if ((*ctx.record_year_hist)[i] > 0) {
                int y = 1900 + (int)i;
                nz_min = std::min(nz_min, y);
                nz_max = std::max(nz_max, y);
                nz_total += (*ctx.record_year_hist)[i];
            }
            if ((*ctx.record_year_hist_plot)[i] > max_bin) max_bin = (*ctx.record_year_hist_plot)[i];
        }
        *ctx.record_year_hist_max_bin = max_bin;
        if (nz_min <= nz_max) {
            *ctx.record_year_nonzero_min = nz_min;
            *ctx.record_year_nonzero_max = nz_max;
        } else {
            *ctx.record_year_nonzero_min = 1900;
            *ctx.record_year_nonzero_max = 2100;
        }
        *ctx.record_year_nonzero_total = nz_total;
        *ctx.selected_record_year_dirty = true;
        *ctx.hist_dirty = false;
    }

    ImGui::TextDisabled("Enabled-layer records by year");
    if (*ctx.record_year_nonzero_total <= 0) {
        ImGui::TextDisabled("No recognized date fields found in currently enabled layers.");
    } else {
        ImGui::Text("Range: %d-%d  Total: %d  Peak: %.0f",
                    *ctx.record_year_nonzero_min,
                    *ctx.record_year_nonzero_max,
                    *ctx.record_year_nonzero_total,
                    *ctx.record_year_hist_max_bin);
        const int plot_offset = std::max(0, *ctx.record_year_nonzero_min - 1900);
        const int plot_count = std::max(1, *ctx.record_year_nonzero_max - *ctx.record_year_nonzero_min + 1);
        ImGui::PlotHistogram(
            "##record_year_hist",
            ctx.record_year_hist_plot->data() + plot_offset,
            plot_count,
            0,
            nullptr,
            0.0f,
            *ctx.record_year_hist_max_bin * 1.05f,
            ImVec2(-1.0f, 140.0f));
    }

    ImGui::BeginChild("year_hist_list", ImVec2(0, 130), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (int y = *ctx.record_year_nonzero_min; y <= *ctx.record_year_nonzero_max; ++y) {
        int c = (*ctx.record_year_hist)[(size_t)(y - 1900)];
        if (c <= 0) continue;
        char label[64];
        std::snprintf(label, sizeof(label), "%d: %d records", y, c);
        if (ImGui::Selectable(label, *ctx.selected_record_year == y)) {
            *ctx.selected_record_year = y;
            *ctx.selected_record_year_dirty = true;
        }
    }
    ImGui::EndChild();

    if (*ctx.selected_record_year >= 1900 && *ctx.selected_record_year <= 2100) {
        if ((*ctx.record_year_hist)[(size_t)(*ctx.selected_record_year - 1900)] <= 0) {
            *ctx.selected_record_year = -1;
            ctx.selected_record_year_samples->clear();
            *ctx.selected_record_year_total = 0;
            *ctx.selected_record_year_dirty = false;
        }
    }
    if (*ctx.selected_record_year >= 1900 && *ctx.selected_record_year <= 2100 && *ctx.selected_record_year_dirty) {
        constexpr size_t kMaxYearSamples = 8;
        ctx.selected_record_year_samples->clear();
        *ctx.selected_record_year_total = 0;
        for (size_t li = 0; li < ctx.layers->size(); ++li) {
            if (!(*ctx.layers)[li].enabled) continue;
            for (const auto& fg : (*ctx.layers)[li].features) {
                std::string ds = firstDisplayProperty(fg, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
                if (ds.empty() || extractYearMaybe(ds) != *ctx.selected_record_year) continue;
                (*ctx.selected_record_year_total)++;
                if (ctx.selected_record_year_samples->size() >= kMaxYearSamples) continue;
                std::string blocklot = firstDisplayProperty(fg, {"BLOCKLOT", "BLOCK_LOT", "LOT"});
                std::string address = firstDisplayProperty(fg, {"FULLADDR", "PROPERTY_ADDRESS", "PREMISEADD", "ADDRESS", "Address", "ADDR"});
                std::string owner = firstDisplayProperty(fg, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"});
                std::string status = firstDisplayProperty(fg, {"STATUS", "STATE", "CASE_STATUS"});
                std::ostringstream row;
                row << (*ctx.layers)[li].name << " | " << ds;
                if (!blocklot.empty()) row << " | BL " << blocklot;
                if (!address.empty()) row << " | " << address;
                if (!owner.empty()) row << " | " << owner;
                if (!status.empty()) row << " | " << status;
                ctx.selected_record_year_samples->push_back(row.str());
            }
        }
        *ctx.selected_record_year_dirty = false;
    }
    if (*ctx.selected_record_year >= 1900 && *ctx.selected_record_year <= 2100) {
        ImGui::SeparatorText("Selected Year Records");
        ImGui::Text("%d: showing %zu of %d records",
                    *ctx.selected_record_year,
                    ctx.selected_record_year_samples->size(),
                    *ctx.selected_record_year_total);
        ImGui::BeginChild("selected_year_records", ImVec2(0, 170), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        if (ctx.selected_record_year_samples->empty()) {
            ImGui::TextDisabled("No sample records available for this year.");
        } else {
            for (const std::string& row : *ctx.selected_record_year_samples) {
                ImGui::TextWrapped("%s", row.c_str());
                ImGui::Separator();
            }
        }
        ImGui::EndChild();
    }

    ImGui::EndTabItem();
}
