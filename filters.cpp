#include "filters.h"

#include "app_utils.h"
#include "feature_props.h"

#include <initializer_list>

namespace {
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

bool isCrimeLayer(const FeatureFilterContext& ctx, size_t layer_idx) {
    return (ctx.crime_nibrs_layer_idx >= 0 && (int)layer_idx == ctx.crime_nibrs_layer_idx) ||
           (ctx.crime_legacy_layer_idx >= 0 && (int)layer_idx == ctx.crime_legacy_layer_idx);
}

bool crimeFeatureMatches(const FeatureFilterContext& ctx, const LayerDef::FeatureGeom& fg) {
    if (!ctx.crime_filter_enabled) return true;
    const std::string desc = toLowerAscii(firstProp(fg, {"Description", "description", "OFFENSE", "UCRDescription"}));
    const std::string code = toLowerAscii(firstProp(fg, {"CrimeCode", "UCR_CODE", "UCRCode"}));
    const std::string dt = firstProp(fg, {"CrimeDateTime", "CrimeDate", "DATE", "RECORD_DATE"});
    if (ctx.crime_filter_use_year) {
        int yr = extractYearMaybe(dt);
        if (yr < 0 || yr < ctx.crime_year_min || yr > ctx.crime_year_max) return false;
    }
    const bool any_type =
        ctx.crime_filter_homicide || ctx.crime_filter_robbery || ctx.crime_filter_assault ||
        ctx.crime_filter_burglary || ctx.crime_filter_theft || ctx.crime_filter_auto_theft ||
        ctx.crime_filter_drug || ctx.crime_filter_shooting;
    if (!any_type) return true;
    auto has = [&](const char* s) { return desc.find(s) != std::string::npos || code.find(s) != std::string::npos; };
    bool ok = false;
    if (ctx.crime_filter_homicide && (has("homicide") || has("murder"))) ok = true;
    if (ctx.crime_filter_robbery && has("robbery")) ok = true;
    if (ctx.crime_filter_assault && (has("assault") || has("aggravated assault"))) ok = true;
    if (ctx.crime_filter_burglary && has("burglary")) ok = true;
    if (ctx.crime_filter_theft && (has("larceny") || has("theft"))) ok = true;
    if (ctx.crime_filter_auto_theft && (has("motor vehicle theft") || has("auto theft") || has("vehicle theft"))) ok = true;
    if (ctx.crime_filter_drug && (has("drug") || has("narcotic"))) ok = true;
    if (ctx.crime_filter_shooting && has("shooting")) ok = true;
    return ok;
}
}

bool isParcelRelatedLayer(const FeatureFilterContext& ctx, size_t layer_idx) {
    return ctx.layers && layer_idx < ctx.layers->size() && (*ctx.layers)[layer_idx].scale == "parcel";
}

bool featurePassesFilters(
    const FeatureFilterContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureGeom& fg) {
    if (isCrimeLayer(ctx, layer_idx)) {
        if (!ctx.filter_enabled && !ctx.crime_filter_enabled) return true;
        return crimeFeatureMatches(ctx, fg);
    }

    const bool parcel_related_layer = isParcelRelatedLayer(ctx, layer_idx);
    const bool selected_owner_filter_active =
        parcel_related_layer &&
        ctx.selected_owners &&
        !ctx.selected_owners->empty() &&
        ctx.real_property_layer_idx >= 0 &&
        ctx.layers &&
        (size_t)ctx.real_property_layer_idx < ctx.layers->size();
    if (!ctx.filter_enabled && !selected_owner_filter_active) return true;

    const LayerDef::FeatureGeom* rp_join = nullptr;
    if (ctx.layers &&
        ctx.real_property_layer_idx >= 0 &&
        (size_t)ctx.real_property_layer_idx < ctx.layers->size() &&
        ctx.real_property_by_blocklot) {
        std::string bl = featureBlockLotJoinKey(fg);
        if (!bl.empty()) {
            auto itrp = ctx.real_property_by_blocklot->find(bl);
            if (itrp != ctx.real_property_by_blocklot->end() &&
                itrp->second < (*ctx.layers)[(size_t)ctx.real_property_layer_idx].features.size()) {
                rp_join = &(*ctx.layers)[(size_t)ctx.real_property_layer_idx].features[itrp->second];
            }
        }
    }

    if (selected_owner_filter_active) {
        std::string owner = ownerNameFor(rp_join);
        if (owner.empty()) owner = ownerNameFor(&fg);
        if (owner.empty() || ctx.selected_owners->find(owner) == ctx.selected_owners->end()) return false;
    }
    if (!ctx.filter_enabled) return true;

    if (ctx.filter_use_date) {
        std::string ds = firstProp(fg, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
        if (ds.empty() && rp_join) ds = firstProp(*rp_join, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
        if (ds.empty()) return false;
        int yr = extractYearMaybe(ds);
        if (yr < 0 || yr < ctx.filter_year_min || yr > ctx.filter_year_max) return false;
    }

    if (!ctx.filter_blocklot.empty()) {
        std::string bl = firstProp(fg, {"BLOCKLOT", "BLOCK_LOT", "LOT"});
        if (!containsCaseInsensitive(bl, ctx.filter_blocklot)) return false;
    }
    if (!ctx.filter_status.empty()) {
        std::string st = firstProp(fg, {"STATUS", "STATE", "CASE_STATUS"});
        if (st.empty() && rp_join) st = firstProp(*rp_join, {"STATUS", "STATE", "CASE_STATUS"});
        if (st.empty() && ctx.parcel_layer_idx >= 0 && (int)layer_idx == ctx.parcel_layer_idx) {
            const int vn = (ctx.parcel_vac_notice_by_feature && feature_idx < ctx.parcel_vac_notice_by_feature->size()) ? (*ctx.parcel_vac_notice_by_feature)[feature_idx] : 0;
            const int vr = (ctx.parcel_vac_rehab_by_feature && feature_idx < ctx.parcel_vac_rehab_by_feature->size()) ? (*ctx.parcel_vac_rehab_by_feature)[feature_idx] : 0;
            st = (vn + vr) > 0 ? "vacant" : "occupied";
        }
        if (!containsCaseInsensitive(st, ctx.filter_status)) return false;
    }
    if (!ctx.filter_address.empty()) {
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
        if (!addressMatchesSearch(ad, ctx.filter_address)) return false;
    }
    if (parcel_related_layer && !ctx.filter_owner.empty()) {
        std::string ow = firstProp(fg, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"});
        if (ow.empty() && rp_join) ow = firstProp(*rp_join, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"});
        if (!containsCaseInsensitive(ow, ctx.filter_owner)) return false;
    }
    if (!ctx.filter_zip.empty()) {
        std::string zp = firstProp(fg, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
        if (zp.empty() && rp_join) zp = firstProp(*rp_join, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
        if (!containsCaseInsensitive(zp, ctx.filter_zip)) return false;
    }
    return true;
}
