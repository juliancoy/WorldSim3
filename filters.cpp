#include "filters.h"

#include "app_utils.h"
#include "feature_props.h"

#include <initializer_list>

namespace {
const MapFilterState kEmptyMapFilterState;

const MapFilterState& mapFilters(const FeatureFilterContext& ctx) {
    return ctx.map_filters ? *ctx.map_filters : kEmptyMapFilterState;
}

std::string firstProp(const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        std::string v = getPropertyValue(fg, k);
        if (!v.empty()) return v;
    }
    return std::string();
}

std::string ownerNameFor(const LayerDef::FeatureGeom* rp) {
    if (!rp) return "";
    std::string o = firstDisplayProperty(*rp, {"OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR"});
    return toLowerAscii(trimDisplayValue(o));
}

const LayerDef::FeatureGeom* joinedRealProperty(const FeatureFilterContext& ctx, const LayerDef::FeatureGeom& fg) {
    if (!ctx.layers ||
        ctx.real_property_layer_idx < 0 ||
        (size_t)ctx.real_property_layer_idx >= ctx.layers->size() ||
        !ctx.real_property_by_blocklot) {
        return nullptr;
    }
    std::string bl = featureBlockLotJoinKey(fg);
    if (bl.empty()) return nullptr;
    auto itrp = ctx.real_property_by_blocklot->find(bl);
    if (itrp == ctx.real_property_by_blocklot->end() ||
        itrp->second >= (*ctx.layers)[(size_t)ctx.real_property_layer_idx].features.size()) {
        return nullptr;
    }
    return &(*ctx.layers)[(size_t)ctx.real_property_layer_idx].features[itrp->second];
}

bool isCrimeLayer(const FeatureFilterContext& ctx, size_t layer_idx) {
    return ctx.crime_nibrs_layer_idx >= 0 && (int)layer_idx == ctx.crime_nibrs_layer_idx;
}

bool crimeFeatureMatches(const FeatureFilterContext& ctx, const LayerDef::FeatureGeom& fg) {
    const CrimeFilterState& crime = mapFilters(ctx).crime;
    if (!crime.enabled) return true;
    const std::string desc = toLowerAscii(firstProp(fg, {"Description", "description", "OFFENSE", "UCRDescription"}));
    const std::string code = toLowerAscii(firstProp(fg, {"CrimeCode", "UCR_CODE", "UCRCode"}));
    const std::string dt = firstProp(fg, {"CrimeDateTime", "CrimeDate", "DATE", "RECORD_DATE"});
    if (crime.use_year) {
        int yr = extractYearMaybe(dt);
        if (yr < 0 || yr < crime.year_min || yr > crime.year_max) return false;
    }
    const bool any_type =
        crime.homicide || crime.robbery || crime.assault ||
        crime.burglary || crime.theft || crime.auto_theft ||
        crime.drug || crime.shooting;
    if (!any_type) return true;
    auto has = [&](const char* s) { return desc.find(s) != std::string::npos || code.find(s) != std::string::npos; };
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

bool resultSetMatches(const FeatureFilterContext& ctx, const FilterResultSet& result_set, size_t layer_idx, size_t feature_idx, const LayerDef::FeatureGeom& fg) {
    if (result_set.features.find(FeatureKey{layer_idx, feature_idx}) != result_set.features.end()) return true;

    if (!result_set.blocklots.empty()) {
        const std::string blocklot = featureBlockLotJoinKey(fg);
        if (!blocklot.empty() && result_set.blocklots.find(blocklot) != result_set.blocklots.end()) return true;
    }

    if (!result_set.owners.empty()) {
        std::string owner = ownerNameFor(&fg);
        if (owner.empty()) owner = ownerNameFor(joinedRealProperty(ctx, fg));
        if (!owner.empty() && result_set.owners.find(owner) != result_set.owners.end()) return true;
    }

    return false;
}

bool resultSetAllows(const FeatureFilterContext& ctx, size_t layer_idx, size_t feature_idx, const LayerDef::FeatureGeom& fg) {
    const FilterResultSet* result_set = ctx.result_set;
    if (!result_set || !result_set->active) return true;
    const bool layer_targeted =
        result_set->layers.empty() ||
        result_set->layers.find(layer_idx) != result_set->layers.end();
    if (!layer_targeted && !isParcelRelatedLayer(ctx, layer_idx)) return true;
    return resultSetMatches(ctx, *result_set, layer_idx, feature_idx, fg);
}
}

bool isParcelRelatedLayer(const FeatureFilterContext& ctx, size_t layer_idx) {
    return ctx.layers &&
        layer_idx < ctx.layers->size() &&
        (*ctx.layers)[layer_idx].scale == "parcel" &&
        (*ctx.layers)[layer_idx].category != LayerDef::Category::Zoning;
}

bool featurePassesFilters(
    const FeatureFilterContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureGeom& fg) {
    const MapFilterState& filters = mapFilters(ctx);
    if (!resultSetAllows(ctx, layer_idx, feature_idx, fg)) return false;

    if (isCrimeLayer(ctx, layer_idx)) {
        if (!filters.enabled && !filters.crime.enabled) return true;
        return crimeFeatureMatches(ctx, fg);
    }

    const bool parcel_related_layer = isParcelRelatedLayer(ctx, layer_idx);
    const bool selected_owner_filter_active =
        parcel_related_layer &&
        !filters.selected_owners.empty() &&
        ctx.real_property_layer_idx >= 0 &&
        ctx.layers &&
        (size_t)ctx.real_property_layer_idx < ctx.layers->size();
    if (!filters.enabled && !selected_owner_filter_active) return true;

    const LayerDef::FeatureGeom* rp_join = joinedRealProperty(ctx, fg);

    if (selected_owner_filter_active) {
        std::string owner = ownerNameFor(rp_join);
        if (owner.empty()) owner = ownerNameFor(&fg);
        if (owner.empty() || filters.selected_owners.find(owner) == filters.selected_owners.end()) return false;
    }
    if (!filters.enabled) return true;

    if (filters.use_date) {
        std::string ds = firstProp(fg, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
        if (ds.empty() && rp_join) ds = firstProp(*rp_join, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
        if (ds.empty()) return false;
        int yr = extractYearMaybe(ds);
        if (yr < 0 || yr < filters.year_min || yr > filters.year_max) return false;
    }

    if (filters.blocklot[0] != '\0') {
        std::string bl = firstProp(fg, {"BLOCKLOT", "BLOCK_LOT", "LOT"});
        if (!containsCaseInsensitive(bl, filters.blocklot)) return false;
    }
    if (filters.status[0] != '\0') {
        std::string st = firstProp(fg, {"STATUS", "STATE", "CASE_STATUS"});
        if (st.empty() && rp_join) st = firstProp(*rp_join, {"STATUS", "STATE", "CASE_STATUS"});
        if (st.empty() && ctx.parcel_layer_idx >= 0 && (int)layer_idx == ctx.parcel_layer_idx) {
            const int vn = (ctx.parcel_vac_notice_by_feature && feature_idx < ctx.parcel_vac_notice_by_feature->size()) ? (*ctx.parcel_vac_notice_by_feature)[feature_idx] : 0;
            const int vr = (ctx.parcel_vac_rehab_by_feature && feature_idx < ctx.parcel_vac_rehab_by_feature->size()) ? (*ctx.parcel_vac_rehab_by_feature)[feature_idx] : 0;
            st = (vn + vr) > 0 ? "vacant" : "occupied";
        }
        if (!containsCaseInsensitive(st, filters.status)) return false;
    }
    if (filters.address[0] != '\0') {
        std::string ad = firstProp(fg, {
            "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
            "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
            "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
        });
        if (ad.empty() && rp_join) {
            ad = firstProp(*rp_join, {
                "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
                "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
                "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
            });
        }
        if (!addressMatchesSearch(ad, filters.address)) return false;
    }
    if (parcel_related_layer && filters.owner[0] != '\0') {
        std::string ow = firstProp(fg, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"});
        if (ow.empty() && rp_join) ow = firstProp(*rp_join, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"});
        if (!containsCaseInsensitive(ow, filters.owner)) return false;
    }
    if (filters.zip[0] != '\0') {
        std::string zp = firstProp(fg, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
        if (zp.empty() && rp_join) zp = firstProp(*rp_join, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
        if (!containsCaseInsensitive(zp, filters.zip)) return false;
    }
    return true;
}

bool queryMapColorForFeature(
    const FeatureFilterContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureGeom& fg,
    float out_color[4]) {
    if (!ctx.query_layers || !out_color) return false;
    for (auto it = ctx.query_layers->rbegin(); it != ctx.query_layers->rend(); ++it) {
        if (!it->enabled || !it->result_set.active) continue;
        if (!resultSetMatches(ctx, it->result_set, layer_idx, feature_idx, fg)) continue;
        out_color[0] = it->color[0];
        out_color[1] = it->color[1];
        out_color[2] = it->color[2];
        out_color[3] = it->color[3];
        return true;
    }
    return false;
}
