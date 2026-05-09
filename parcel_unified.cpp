#include "parcel_unified.h"

#include "app_utils.h"
#include "feature_props.h"

#include <algorithm>

namespace {
std::string prop(const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys) {
    return firstDisplayProperty(fg, keys);
}

double money(const LayerDef::FeatureGeom* fg, std::initializer_list<const char*> keys) {
    return fg ? parseNumericField(prop(*fg, keys)) : 0.0;
}

std::string ownerDisplay(const LayerDef::FeatureGeom* rp, const LayerDef::FeatureGeom& parcel) {
    std::string owner = rp ? prop(*rp, {"owner", "owner_name", "OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR"}) : "";
    if (owner.empty()) owner = prop(parcel, {"owner", "owner_name", "OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR"});
    return trimDisplayValue(owner);
}

std::string addressFor(const LayerDef::FeatureGeom* rp, const LayerDef::FeatureGeom& parcel) {
    std::string address = prop(parcel, {
        "address", "property_address", "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
        "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
        "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
    });
    if (address.empty() && rp) {
        address = prop(*rp, {
            "address", "property_address", "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
            "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
            "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
        });
    }
    return trimDisplayValue(address);
}

template <typename T>
T vectorValueAt(const std::vector<T>* values, size_t idx, T fallback = T{}) {
    return values && idx < values->size() ? (*values)[idx] : fallback;
}
}

std::vector<UnifiedParcelRecord> buildUnifiedParcels(const UnifiedParcelBuildRequest& request) {
    std::vector<UnifiedParcelRecord> out;
    if (!request.layers ||
        request.parcel_layer_idx < 0 ||
        (size_t)request.parcel_layer_idx >= request.layers->size()) {
        return out;
    }

    const auto& parcel_layer = (*request.layers)[(size_t)request.parcel_layer_idx];
    out.reserve(parcel_layer.features.size());
    for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
        const auto& parcel = parcel_layer.features[i];
        UnifiedParcelRecord row;
        row.parcel_layer_idx = (size_t)request.parcel_layer_idx;
        row.parcel_feature_idx = i;
        row.parcel_geom = &parcel;
        row.blocklot = featureBlockLotJoinKey(parcel);

        if (!row.blocklot.empty() &&
            request.real_property_by_blocklot &&
            request.real_property_layer_idx >= 0 &&
            (size_t)request.real_property_layer_idx < request.layers->size()) {
            auto it = request.real_property_by_blocklot->find(row.blocklot);
            const auto& rp_layer = (*request.layers)[(size_t)request.real_property_layer_idx];
            if (it != request.real_property_by_blocklot->end() && it->second < rp_layer.features.size()) {
                row.real_property_feature_idx = it->second;
                row.real_property = &rp_layer.features[it->second];
            }
        }

        row.owner_display = ownerDisplay(row.real_property, parcel);
        row.owner = toLowerAscii(row.owner_display);
        row.address = addressFor(row.real_property, parcel);
        row.zip = row.real_property ? prop(*row.real_property, {"zip", "ZIP", "ZIPCODE", "POSTAL_CODE"}) : "";
        if (row.zip.empty()) row.zip = prop(parcel, {"zip", "ZIP", "ZIPCODE", "POSTAL_CODE"});
        row.status = row.real_property ? prop(*row.real_property, {"STATUS", "STATE", "CASE_STATUS"}) : "";
        if (row.status.empty()) row.status = prop(parcel, {"STATUS", "STATE", "CASE_STATUS"});

        row.current_land = money(row.real_property, {"land_value", "CURRLAND"});
        if (row.current_land <= 0.0) row.current_land = money(&parcel, {"land_value", "CURRLAND"});
        row.current_improvements = money(row.real_property, {"improvement_value", "CURRIMPR"});
        if (row.current_improvements <= 0.0) row.current_improvements = money(&parcel, {"improvement_value", "CURRIMPR"});
        row.tax_base = money(row.real_property, {"current_value", "tax_base", "TAXBASE", "ARTAXBAS"});
        if (row.tax_base <= 0.0) row.tax_base = money(&parcel, {"current_value", "tax_base", "TAXBASE", "ARTAXBAS"});
        row.sale_price = money(row.real_property, {"sale_price", "SALEPRIC"});
        if (row.sale_price <= 0.0) row.sale_price = money(&parcel, {"sale_price", "SALEPRIC"});
        if (row.tax_base > 0.0) row.current_value = row.tax_base;
        else if (row.current_land + row.current_improvements > 0.0) row.current_value = row.current_land + row.current_improvements;
        else row.current_value = std::max(0.0, row.sale_price);

        row.vacant_notice_count = vectorValueAt(request.parcel_vac_notice_by_feature, i, 0);
        row.vacant_rehab_count = vectorValueAt(request.parcel_vac_rehab_by_feature, i, 0);
        row.tax_lien_count = vectorValueAt(request.parcel_tax_lien_by_feature, i, 0);
        row.tax_sale_count = vectorValueAt(request.parcel_tax_sale_by_feature, i, 0);
        row.tax_lien_amount = vectorValueAt(request.parcel_tax_lien_amount_by_feature, i, 0.0);
        row.tax_sale_amount = vectorValueAt(request.parcel_tax_sale_amount_by_feature, i, 0.0);

        out.push_back(std::move(row));
    }
    return out;
}

const UnifiedParcelRecord* unifiedParcelAt(const std::vector<UnifiedParcelRecord>& parcels, size_t parcel_feature_idx) {
    if (parcel_feature_idx >= parcels.size()) return nullptr;
    const UnifiedParcelRecord& row = parcels[parcel_feature_idx];
    return row.parcel_feature_idx == parcel_feature_idx ? &row : nullptr;
}
