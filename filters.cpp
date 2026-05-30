#include "filters.h"

#include "app_utils.h"
#include "event_sectors.h"
#include "feature_props.h"
#include "real_property_ui.h"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <cstring>

namespace {
const MapFilterState kEmptyMapFilterState;

const LayerDef* layerAt(const FeatureFilterContext& ctx, size_t layer_idx) {
    if (!ctx.layers || layer_idx >= ctx.layers->size()) return nullptr;
    return &(*ctx.layers)[layer_idx];
}

void ensureNormalizedLayerGeographyCache(const LayerDef& layer) {
    if (layer.normalized_geography_cache_valid) return;
    layer.normalized_provenance_nation_state = normalizeGeographyToken(layer.provenance_nation_state);
    layer.normalized_provenance_state_region = normalizeGeographyToken(layer.provenance_state_region);
    layer.normalized_provenance_county_city = normalizeGeographyToken(layer.provenance_county_city);
    layer.normalized_geography_cache_valid = true;
}

const MapFilterState& mapFilters(const FeatureFilterContext& ctx) {
    return ctx.map_filters ? *ctx.map_filters : kEmptyMapFilterState;
}

const LayerDef::FeatureRecord* joinedRealProperty(const FeatureFilterContext& ctx, const LayerDef::FeatureRecord& fg) {
    if (!ctx.layers ||
        ctx.real_property_layer_idx < 0 ||
        (size_t)ctx.real_property_layer_idx >= ctx.layers->size()) {
        return nullptr;
    }
    const size_t joined_idx = [&]() -> size_t {
        if (!ctx.real_property_by_blocklot) return (size_t)-1;
        std::string bl = featureBlockLotJoinKey(fg);
        if (bl.empty()) return (size_t)-1;
        auto itrp = ctx.real_property_by_blocklot->find(bl);
        if (itrp == ctx.real_property_by_blocklot->end()) return (size_t)-1;
        return itrp->second;
    }();
    if (joined_idx >= (*ctx.layers)[(size_t)ctx.real_property_layer_idx].features.size()) return nullptr;
    return &(*ctx.layers)[(size_t)ctx.real_property_layer_idx].features[joined_idx];
}

size_t joinedRealPropertyIndex(const FeatureFilterContext& ctx, const LayerDef::FeatureRecord& fg) {
    if (!ctx.layers ||
        ctx.real_property_layer_idx < 0 ||
        (size_t)ctx.real_property_layer_idx >= ctx.layers->size() ||
        !ctx.real_property_by_blocklot) {
        return (size_t)-1;
    }
    std::string bl = featureBlockLotJoinKey(fg);
    if (bl.empty()) return (size_t)-1;
    auto itrp = ctx.real_property_by_blocklot->find(bl);
    if (itrp == ctx.real_property_by_blocklot->end() ||
        itrp->second >= (*ctx.layers)[(size_t)ctx.real_property_layer_idx].features.size()) {
        return (size_t)-1;
    }
    return itrp->second;
}

std::string ownerSearchText(
    const FeatureFilterContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg,
    const LayerDef::FeatureRecord* rp_join,
    size_t rp_join_idx) {
    if (ctx.parcel_layer_idx >= 0 &&
        (int)layer_idx == ctx.parcel_layer_idx &&
        ctx.parcel_owner_search_by_feature &&
        feature_idx < ctx.parcel_owner_search_by_feature->size()) {
        return (*ctx.parcel_owner_search_by_feature)[feature_idx];
    }
    if (ctx.real_property_layer_idx >= 0 &&
        (int)layer_idx == ctx.real_property_layer_idx &&
        ctx.real_property_owner_search_by_feature &&
        feature_idx < ctx.real_property_owner_search_by_feature->size()) {
        return (*ctx.real_property_owner_search_by_feature)[feature_idx];
    }
    if (rp_join_idx != (size_t)-1 &&
        ctx.real_property_owner_search_by_feature &&
        rp_join_idx < ctx.real_property_owner_search_by_feature->size()) {
        return (*ctx.real_property_owner_search_by_feature)[rp_join_idx];
    }

    std::string owner = firstDisplayProperty(fg, {
        "owner", "owner_name",
        "OWNER_1", "OWNER_2", "OWNER_3",
        "OWNERNME1", "OWNER", "OWNER_NAME",
        "AR_OWNER", "OWNER_ABBR"
    });
    if (owner.empty() && rp_join) {
        owner = firstDisplayProperty(*rp_join, {
            "owner", "owner_name",
            "OWNER_1", "OWNER_2", "OWNER_3",
            "OWNERNME1", "OWNER", "OWNER_NAME",
            "AR_OWNER", "OWNER_ABBR"
        });
    }
    return normalizeFuzzySearchText(owner);
}

bool isCrimeLayer(const FeatureFilterContext& ctx, size_t layer_idx) {
    return ctx.crime_nibrs_layer_idx >= 0 && (int)layer_idx == ctx.crime_nibrs_layer_idx;
}

bool isZoningLayer(const FeatureFilterContext& ctx, size_t layer_idx) {
    if (!ctx.layers || layer_idx >= ctx.layers->size()) return false;
    const LayerDef& layer = (*ctx.layers)[layer_idx];
    if (layerUsesPointGeometry(layer)) return false;
    if (layer.category == LayerDef::Category::Zoning) return true;
    return containsCaseInsensitive(layer.file, "zoning") ||
           containsCaseInsensitive(layer.name, "zoning");
}

bool crimeFeatureMatches(const FeatureFilterContext& ctx, const LayerDef::FeatureRecord& fg) {
    const CrimeFilterState& crime = mapFilters(ctx).crime;
    if (!crime.enabled) return true;
    const std::string desc = firstDisplayProperty(fg, {"Description", "description", "OFFENSE", "UCRDescription"});
    const std::string code = firstDisplayProperty(fg, {"CrimeCode", "UCR_CODE", "UCRCode"});
    const std::string dt = firstDisplayProperty(fg, {"CrimeDateTime", "CrimeDate", "DATE", "RECORD_DATE"});
    if (crime.use_year) {
        int yr = extractYearMaybe(dt);
        if (yr < 0 || yr < crime.year_min || yr > crime.year_max) return false;
    }
    const bool any_type =
        crime.homicide || crime.robbery || crime.assault ||
        crime.burglary || crime.theft || crime.auto_theft ||
        crime.drug || crime.shooting;
    if (!any_type) return true;
    auto has = [&](const char* s) { return containsCaseInsensitive(desc, s) || containsCaseInsensitive(code, s); };
    bool ok = false;
    if (crime.homicide && (has("homicide") || has("murder"))) ok = true;
    if (crime.robbery && has("robbery")) ok = true;
    if (crime.assault && (has("assault") || has("aggravated assault"))) ok = true;
    if (crime.burglary && has("burglary")) ok = true;
    if (crime.theft && (has("larceny") || has("theft"))) ok = true;
    if (crime.auto_theft && (has("motor vehicle theft") || has("auto theft") || has("vehicle theft"))) ok = true;
    if (crime.drug && (has("drug") || has("narcotic"))) ok = true;
    if (crime.shooting && has("shooting")) ok = true;
    return ok;
}

bool crimeFeatureMatches(const FeatureFilterContext& ctx, size_t layer_idx, size_t feature_idx, const LayerDef::FeatureRecord& fg) {
    const LayerDef* layer = layerAt(ctx, layer_idx);
    if (!layer) return crimeFeatureMatches(ctx, fg);
    const CrimeFilterState& crime = mapFilters(ctx).crime;
    if (!crime.enabled) return true;
    const std::string desc = firstDisplayProperty(*layer, feature_idx, {"Description", "description", "OFFENSE", "UCRDescription"});
    const std::string code = firstDisplayProperty(*layer, feature_idx, {"CrimeCode", "UCR_CODE", "UCRCode"});
    const std::string dt = firstDisplayProperty(*layer, feature_idx, {"CrimeDateTime", "CrimeDate", "DATE", "RECORD_DATE"});
    if (crime.use_year) {
        int yr = extractYearMaybe(dt);
        if (yr < 0 || yr < crime.year_min || yr > crime.year_max) return false;
    }
    const bool any_type =
        crime.homicide || crime.robbery || crime.assault ||
        crime.burglary || crime.theft || crime.auto_theft ||
        crime.drug || crime.shooting;
    if (!any_type) return true;
    auto has = [&](const char* s) { return containsCaseInsensitive(desc, s) || containsCaseInsensitive(code, s); };
    bool ok = false;
    if (crime.homicide && (has("homicide") || has("murder"))) ok = true;
    if (crime.robbery && has("robbery")) ok = true;
    if (crime.assault && (has("assault") || has("aggravated assault"))) ok = true;
    if (crime.burglary && has("burglary")) ok = true;
    if (crime.theft && (has("larceny") || has("theft"))) ok = true;
    if (crime.auto_theft && (has("motor vehicle theft") || has("auto theft") || has("vehicle theft"))) ok = true;
    if (crime.drug && (has("drug") || has("narcotic"))) ok = true;
    if (crime.shooting && has("shooting")) ok = true;
    return ok;
}

bool resultSetMatches(const FeatureFilterContext& ctx, const FilterResultSet& result_set, size_t layer_idx, size_t feature_idx, const LayerDef::FeatureRecord& fg) {
    if (result_set.features.find(FeatureKey{layer_idx, feature_idx}) != result_set.features.end()) return true;

    if (!result_set.blocklots.empty()) {
        const std::string blocklot = featureBlockLotJoinKey(fg);
        if (!blocklot.empty() && result_set.blocklots.find(blocklot) != result_set.blocklots.end()) return true;
    }

    if (!result_set.owners.empty()) {
        std::string owner = normalizedRealPropertyOwnerName(&fg);
        if (owner.empty()) owner = normalizedRealPropertyOwnerName(joinedRealProperty(ctx, fg));
        if (!owner.empty() && result_set.owners.find(owner) != result_set.owners.end()) return true;
    }

    return false;
}

bool resultSetAllows(const FeatureFilterContext& ctx, size_t layer_idx, size_t feature_idx, const LayerDef::FeatureRecord& fg) {
    auto allows_one = [&](const FilterResultSet* result_set) {
        if (!result_set || !result_set->active) return true;
        const bool layer_targeted =
            result_set->layers.empty() ||
            result_set->layers.find(layer_idx) != result_set->layers.end();
        if (!layer_targeted && !isParcelRelatedLayer(ctx, layer_idx)) return true;
        return resultSetMatches(ctx, *result_set, layer_idx, feature_idx, fg);
    };
    return allows_one(ctx.result_set) &&
        allows_one(ctx.secondary_result_set) &&
        allows_one(ctx.tertiary_result_set);
}

void hashMix(uint64_t& h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
}

void hashCString(uint64_t& h, const char* s) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s ? s : "");
    while (*p) {
        hashMix(h, *p);
        ++p;
    }
}

void hashString(uint64_t& h, const std::string& value) {
    hashCString(h, value.c_str());
}

void hashFloat(uint64_t& h, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    hashMix(h, bits);
}

void hashResultSet(uint64_t& h, const FilterResultSet* result_set) {
    if (!result_set) {
        hashMix(h, 0);
        return;
    }
    hashMix(h, 1);
    hashMix(h, (uint64_t)result_set->active);
    hashMix(h, (uint64_t)result_set->layers.size());
    hashMix(h, (uint64_t)result_set->features.size());
    hashMix(h, (uint64_t)result_set->blocklots.size());
    hashMix(h, (uint64_t)result_set->owners.size());
    std::vector<size_t> layers(result_set->layers.begin(), result_set->layers.end());
    std::sort(layers.begin(), layers.end());
    for (size_t layer_idx : layers) hashMix(h, (uint64_t)layer_idx);

    std::vector<FeatureKey> features(result_set->features.begin(), result_set->features.end());
    std::sort(features.begin(), features.end(), [](const FeatureKey& a, const FeatureKey& b) {
        if (a.layer_idx != b.layer_idx) return a.layer_idx < b.layer_idx;
        return a.feature_idx < b.feature_idx;
    });
    for (const FeatureKey& feature : features) {
        hashMix(h, (uint64_t)feature.layer_idx);
        hashMix(h, (uint64_t)feature.feature_idx);
    }

    std::vector<std::string> blocklots(result_set->blocklots.begin(), result_set->blocklots.end());
    std::sort(blocklots.begin(), blocklots.end());
    for (const std::string& blocklot : blocklots) hashString(h, blocklot);

    std::vector<std::string> owners(result_set->owners.begin(), result_set->owners.end());
    std::sort(owners.begin(), owners.end());
    for (const std::string& owner : owners) hashString(h, owner);
}
}

bool isParcelRelatedLayer(const FeatureFilterContext& ctx, size_t layer_idx) {
    return ctx.layers &&
        layer_idx < ctx.layers->size() &&
        (*ctx.layers)[layer_idx].scale == "parcel" &&
        (*ctx.layers)[layer_idx].category != LayerDef::Category::Zoning;
}

bool layerMatchesBrowseGeography(const LayerDef& layer, const LayerBrowseState& browse_state) {
    ensureNormalizedLayerGeographyCache(layer);
    const std::string selected_nation = normalizeGeographyToken(browse_state.selected_nation_state);
    const std::string selected_region = normalizeGeographyToken(browse_state.selected_state_region);
    if (!selected_nation.empty()) {
        if (!layer.normalized_provenance_nation_state.empty() &&
            layer.normalized_provenance_nation_state != selected_nation) {
            return false;
        }
    }
    if (!selected_region.empty()) {
        if (!layer.normalized_provenance_state_region.empty() &&
            layer.normalized_provenance_state_region != selected_region) {
            return false;
        }
    }
    return true;
}

bool featurePassesFilters(
    const FeatureFilterContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg) {
    const MapFilterState& filters = mapFilters(ctx);
    if (!resultSetAllows(ctx, layer_idx, feature_idx, fg)) return false;
    const LayerDef* layer = layerAt(ctx, layer_idx);

    if (isCrimeLayer(ctx, layer_idx)) {
        if (!filters.enabled && !filters.crime.enabled) return true;
        return crimeFeatureMatches(ctx, layer_idx, feature_idx, fg);
    }

    if (ctx.layers && layer_idx < ctx.layers->size() && isCommunitySectorEventLayer((*ctx.layers)[layer_idx])) {
        if (!filters.event_sector_enabled.empty()) {
            const std::string sector = classifyCommunitySector(fg);
            auto it = filters.event_sector_enabled.find(sector);
            if (it != filters.event_sector_enabled.end() && !it->second) return false;
        }
    }

    const bool zoning_layer = isZoningLayer(ctx, layer_idx);

    const bool parcel_related_layer = isParcelRelatedLayer(ctx, layer_idx);
    const bool selected_owner_filter_active =
        parcel_related_layer &&
        !filters.selected_owners.empty() &&
        ctx.real_property_layer_idx >= 0 &&
        ctx.layers &&
        (size_t)ctx.real_property_layer_idx < ctx.layers->size();
    if (!filters.enabled && !selected_owner_filter_active) return true;

    const size_t rp_join_idx = joinedRealPropertyIndex(ctx, fg);
    const LayerDef::FeatureRecord* rp_join =
        rp_join_idx == (size_t)-1 ? nullptr
                                  : &(*ctx.layers)[(size_t)ctx.real_property_layer_idx].features[rp_join_idx];

    if (selected_owner_filter_active) {
        std::string owner = normalizedRealPropertyOwnerName(rp_join);
        if (owner.empty()) owner = normalizedRealPropertyOwnerName(&fg);
        if (owner.empty() || filters.selected_owners.find(owner) == filters.selected_owners.end()) return false;
    }
    if (!filters.enabled) return true;

    if (zoning_layer) return true;

    if (filters.use_date) {
        std::string ds = layer ? firstDisplayProperty(*layer, feature_idx, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"})
                               : firstDisplayProperty(fg, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
        if (ds.empty() && rp_join) ds = firstDisplayProperty(*rp_join, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
        if (ds.empty()) return false;
        int yr = extractYearMaybe(ds);
        if (yr < 0 || yr < filters.year_min || yr > filters.year_max) return false;
    }

    if (filters.blocklot[0] != '\0') {
        std::string bl = layer ? firstDisplayProperty(*layer, feature_idx, {"BLOCKLOT", "BLOCK_LOT", "LOT"})
                               : firstDisplayProperty(fg, {"BLOCKLOT", "BLOCK_LOT", "LOT"});
        if (!containsCaseInsensitive(bl, filters.blocklot)) return false;
    }
    if (filters.status[0] != '\0') {
        std::string st = layer ? firstDisplayProperty(*layer, feature_idx, {"STATUS", "STATE", "CASE_STATUS"})
                               : firstDisplayProperty(fg, {"STATUS", "STATE", "CASE_STATUS"});
        if (st.empty() && rp_join) st = firstDisplayProperty(*rp_join, {"STATUS", "STATE", "CASE_STATUS"});
        if (st.empty() && ctx.parcel_layer_idx >= 0 && (int)layer_idx == ctx.parcel_layer_idx) {
            const int vn = (ctx.parcel_vac_notice_by_feature && feature_idx < ctx.parcel_vac_notice_by_feature->size()) ? (*ctx.parcel_vac_notice_by_feature)[feature_idx] : 0;
            const int vr = (ctx.parcel_vac_rehab_by_feature && feature_idx < ctx.parcel_vac_rehab_by_feature->size()) ? (*ctx.parcel_vac_rehab_by_feature)[feature_idx] : 0;
            st = (vn + vr) > 0 ? "vacant" : "occupied";
        }
        if (!containsCaseInsensitive(st, filters.status)) return false;
    }
    if (filters.address[0] != '\0' && !ctx.compiled_address_filter_active) {
        std::string ad = layer ? firstDisplayProperty(*layer, feature_idx, {
            "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
            "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
            "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
        }) : firstDisplayProperty(fg, {
            "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
            "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
            "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
        });
        if (ad.empty() && rp_join) {
            ad = firstDisplayProperty(*rp_join, {
                "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
                "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
                "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
            });
        }
        if (!addressMatchesSearch(ad, filters.address)) return false;
    }
    if (parcel_related_layer && !ctx.owner_filter_normalized.empty() && !ctx.compiled_owner_filter_active) {
        const auto owner_filter_begin = std::chrono::steady_clock::now();
        const std::string owner_search = ownerSearchText(ctx, layer_idx, feature_idx, fg, rp_join, rp_join_idx);
        if (ctx.owner_filter_candidates_accum) {
            *ctx.owner_filter_candidates_accum += 1;
        }
        const bool matched = !owner_search.empty() &&
            owner_search.find(ctx.owner_filter_normalized) != std::string::npos;
        if (ctx.owner_filter_matches_accum && matched) {
            *ctx.owner_filter_matches_accum += 1;
        }
        if (ctx.owner_filter_ms_accum) {
            *ctx.owner_filter_ms_accum += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - owner_filter_begin).count();
        }
        if (!matched) return false;
    }
    if (filters.zip[0] != '\0') {
        std::string zp = layer ? firstDisplayProperty(*layer, feature_idx, {"ZIP", "ZIPCODE", "POSTAL_CODE"})
                               : firstDisplayProperty(fg, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
        if (zp.empty() && rp_join) zp = firstDisplayProperty(*rp_join, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
        if (!containsCaseInsensitive(zp, filters.zip)) return false;
    }
    return true;
}

bool queryMapColorForFeature(
    const FeatureFilterContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg,
    float out_color[4]) {
    return queryMapStyleForFeature(ctx, layer_idx, feature_idx, fg, out_color, nullptr);
}

bool queryMapStyleForFeature(
    const FeatureFilterContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg,
    float out_color[4],
    float out_outline_color[4]) {
    if (!ctx.query_layers || !out_color) return false;
    for (auto it = ctx.query_layers->rbegin(); it != ctx.query_layers->rend(); ++it) {
        if (!it->enabled || !it->result_set.active) continue;
        if (!resultSetMatches(ctx, it->result_set, layer_idx, feature_idx, fg)) continue;
        out_color[0] = it->color[0];
        out_color[1] = it->color[1];
        out_color[2] = it->color[2];
        out_color[3] = it->color[3];
        if (out_outline_color) {
            out_outline_color[0] = it->outline_color[0];
            out_outline_color[1] = it->outline_color[1];
            out_outline_color[2] = it->outline_color[2];
            out_outline_color[3] = it->outline_color[3];
        }
        return true;
    }
    return false;
}

uint64_t buildFeatureRenderStateKey(const FeatureRenderStateKeyContext& ctx) {
    uint64_t state_key = 1469598103934665603ULL;
    const MapFilterState& filters = ctx.map_filters ? *ctx.map_filters : kEmptyMapFilterState;
    hashMix(state_key, (uint64_t)filters.enabled);
    hashMix(state_key, (uint64_t)filters.use_date);
    hashMix(state_key, (uint64_t)filters.year_min);
    hashMix(state_key, (uint64_t)filters.year_max);
    hashCString(state_key, filters.blocklot);
    hashCString(state_key, filters.status);
    hashCString(state_key, filters.address);
    hashCString(state_key, filters.owner);
    hashCString(state_key, filters.zip);
    std::vector<std::string> selected_owners(filters.selected_owners.begin(), filters.selected_owners.end());
    std::sort(selected_owners.begin(), selected_owners.end());
    hashMix(state_key, (uint64_t)selected_owners.size());
    for (const std::string& owner : selected_owners) hashString(state_key, owner);
    std::vector<std::pair<std::string, bool>> event_sectors(
        filters.event_sector_enabled.begin(),
        filters.event_sector_enabled.end());
    std::sort(event_sectors.begin(), event_sectors.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    hashMix(state_key, (uint64_t)event_sectors.size());
    for (const auto& [sector, enabled] : event_sectors) {
        hashString(state_key, sector);
        hashMix(state_key, (uint64_t)enabled);
    }
    hashMix(state_key, (uint64_t)filters.crime.enabled);
    hashMix(state_key, (uint64_t)filters.crime.homicide);
    hashMix(state_key, (uint64_t)filters.crime.robbery);
    hashMix(state_key, (uint64_t)filters.crime.assault);
    hashMix(state_key, (uint64_t)filters.crime.burglary);
    hashMix(state_key, (uint64_t)filters.crime.theft);
    hashMix(state_key, (uint64_t)filters.crime.auto_theft);
    hashMix(state_key, (uint64_t)filters.crime.drug);
    hashMix(state_key, (uint64_t)filters.crime.shooting);
    hashMix(state_key, (uint64_t)filters.crime.use_year);
    hashMix(state_key, (uint64_t)filters.crime.year_min);
    hashMix(state_key, (uint64_t)filters.crime.year_max);
    hashResultSet(state_key, ctx.result_set);
    hashResultSet(state_key, ctx.secondary_result_set);
    hashResultSet(state_key, ctx.tertiary_result_set);
    hashMix(state_key, (uint64_t)(ctx.query_layers ? ctx.query_layers->size() : 0));
    if (ctx.query_layers) {
        for (const QueryMapLayer& layer : *ctx.query_layers) {
            hashMix(state_key, (uint64_t)layer.enabled);
            hashMix(state_key, (uint64_t)layer.result_set.active);
            hashMix(state_key, (uint64_t)layer.row_count);
            hashMix(state_key, (uint64_t)layer.result_set.layers.size());
            hashMix(state_key, (uint64_t)layer.result_set.features.size());
            hashMix(state_key, (uint64_t)layer.result_set.blocklots.size());
            hashMix(state_key, (uint64_t)layer.result_set.owners.size());
            hashResultSet(state_key, &layer.result_set);
            for (float color : layer.color) hashFloat(state_key, color);
            for (float color : layer.outline_color) hashFloat(state_key, color);
        }
    }
    return state_key;
}

bool ensureLayerFeatureRenderCache(
    const FeatureFilterContext& ctx,
    const std::vector<LayerDef>& layers,
    uint64_t state_key,
    LayerFeatureRenderCache& cache) {
    if (cache.state_key == state_key &&
        cache.layer_states.size() == layers.size()) {
        bool shape_ok = true;
        for (size_t i = 0; i < layers.size(); ++i) {
            if (cache.layer_states[i].size() != layers[i].features.size()) {
                shape_ok = false;
                break;
            }
        }
        if (shape_ok) return false;
    }

    cache.layer_states.clear();
    cache.layer_states.resize(layers.size());
    for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
        const LayerDef& layer = layers[layer_idx];
        auto& feature_states = cache.layer_states[layer_idx];
        feature_states.resize(layer.features.size());
        for (size_t feature_idx = 0; feature_idx < layer.features.size(); ++feature_idx) {
            const LayerDef::FeatureRecord& fg = layer.features[feature_idx];
            FeatureRenderState state;
            state.visible = featurePassesFilters(ctx, layer_idx, feature_idx, fg);
            if (state.visible) {
                float query_color[4] = {0, 0, 0, 0};
                float query_outline_color[4] = {0, 0, 0, 0};
                if (queryMapStyleForFeature(ctx, layer_idx, feature_idx, fg, query_color, query_outline_color)) {
                    state.has_query_color = true;
                    state.query_color = ImGui::ColorConvertFloat4ToU32(
                        ImVec4(query_color[0], query_color[1], query_color[2], query_color[3]));
                    state.has_query_outline_color = true;
                    state.query_outline_color = ImGui::ColorConvertFloat4ToU32(
                        ImVec4(query_outline_color[0], query_outline_color[1], query_outline_color[2], query_outline_color[3]));
                }
            }
            feature_states[feature_idx] = state;
        }
    }
    cache.state_key = state_key;
    return true;
}

const FeatureRenderState* findFeatureRenderState(
    const LayerFeatureRenderCache& cache,
    size_t layer_idx,
    size_t feature_idx) {
    if (layer_idx >= cache.layer_states.size()) return nullptr;
    const auto& layer_states = cache.layer_states[layer_idx];
    if (feature_idx >= layer_states.size()) return nullptr;
    return &layer_states[feature_idx];
}
