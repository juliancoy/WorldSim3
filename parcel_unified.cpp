#include "parcel_unified.h"

#include "app_utils.h"
#include "feature_props.h"
#include "parcel_metrics.h"

#include <algorithm>

namespace {
double money(const LayerDef::FeatureRecord* fg, std::initializer_list<const char*> keys) {
    return fg ? parseNumericField(firstDisplayProperty(*fg, keys)) : 0.0;
}

std::string ownerDisplay(const LayerDef::FeatureRecord* rp, const LayerDef::FeatureRecord& parcel) {
    std::string owner = rp ? firstDisplayProperty(*rp, {"owner", "owner_name", "OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR"}) : "";
    if (owner.empty()) owner = firstDisplayProperty(parcel, {"owner", "owner_name", "OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR"});
    return trimDisplayValue(owner);
}

std::string addressFor(const LayerDef::FeatureRecord* rp, const LayerDef::FeatureRecord& parcel) {
    std::string address = firstDisplayProperty(parcel, {
        "address", "property_address", "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
        "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
        "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
    });
    if (address.empty() && rp) {
        address = firstDisplayProperty(*rp, {
            "address", "property_address", "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
            "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
            "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
        });
    }
    return trimDisplayValue(address);
}

std::string sourceOfTruthForFeature(
    const LayerDef::FeatureRecord* feature,
    const std::string& fallback) {
    if (feature) {
        std::string source = trimDisplayValue(firstDisplayProperty(*feature, {
            "source_file", "SOURCE_FILE", "source", "SOURCE"
        }));
        if (!source.empty()) return source;
    }
    return trimDisplayValue(fallback);
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
    const std::vector<LayerDef::FeatureRecord>* real_property_features = request.real_property_features;
    if (!real_property_features &&
        request.real_property_layer_idx >= 0 &&
        (size_t)request.real_property_layer_idx < request.layers->size()) {
        real_property_features = &(*request.layers)[(size_t)request.real_property_layer_idx].features;
    }
    out.reserve(parcel_layer.features.size());
    for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
        const auto& parcel = parcel_layer.features[i];
        UnifiedParcelRecord row;
        row.parcel_layer_idx = (size_t)request.parcel_layer_idx;
        row.parcel_local_feature_idx = i;
        row.parcel_entity_id = featureEntityIdForLayerFeature(parcel_layer, parcel, i);
        row.parcel_geometry_entity_id = featureGeometryEntityIdForLayerFeature(parcel_layer, parcel, i);
        row.blocklot = featureBlockLotJoinKey(parcel);
        row.parcel_source_file = sourceOfTruthForFeature(&parcel, parcel_layer.file);
        row.parcel_has_geometry = parcelHasGeometry(request.parcel_render_blob, i, &parcel);
        if (!parcelExtent(request.parcel_render_blob, i, &parcel, row.parcel_extent)) {
            row.parcel_extent = {};
        }
        row.real_property_layer_idx = request.real_property_layer_idx;

        if (!row.blocklot.empty() &&
            request.real_property_by_blocklot &&
            real_property_features) {
            auto it = request.real_property_by_blocklot->find(row.blocklot);
            if (it != request.real_property_by_blocklot->end() && it->second < real_property_features->size()) {
                row.real_property_local_feature_idx = it->second;
                row.has_property_record = true;
                if (request.real_property_source_files && it->second < request.real_property_source_files->size()) {
                    row.property_source_file = (*request.real_property_source_files)[it->second];
                }
                if (row.property_source_file.empty()) {
                    row.property_source_file = sourceOfTruthForFeature(&(*real_property_features)[it->second], {});
                }
            }
        }

        const LayerDef::FeatureRecord* rp = row.real_property_local_feature_idx != (size_t)-1
            ? &(*real_property_features)[row.real_property_local_feature_idx]
            : nullptr;
        row.owner_display = ownerDisplay(rp, parcel);
        row.owner = canonicalOwnerName(row.owner_display);
        row.owner_search = normalizeFuzzySearchText(row.owner_display.empty() ? row.owner : row.owner_display);
        row.address = addressFor(rp, parcel);
        row.address_search = normalizeAddressSearchText(row.address);
        row.zip = rp ? firstDisplayProperty(*rp, {"zip", "ZIP", "ZIPCODE", "POSTAL_CODE"}) : "";
        if (row.zip.empty()) row.zip = firstDisplayProperty(parcel, {"zip", "ZIP", "ZIPCODE", "POSTAL_CODE"});
        row.status = rp ? firstDisplayProperty(*rp, {"STATUS", "STATE", "CASE_STATUS"}) : "";
        if (row.status.empty()) row.status = firstDisplayProperty(parcel, {"STATUS", "STATE", "CASE_STATUS"});

        row.current_land = money(rp, {"land_value", "CURRLAND"});
        if (row.current_land <= 0.0) row.current_land = money(&parcel, {"land_value", "CURRLAND"});
        row.current_improvements = money(rp, {"improvement_value", "CURRIMPR"});
        if (row.current_improvements <= 0.0) row.current_improvements = money(&parcel, {"improvement_value", "CURRIMPR"});
        row.structure_area_sqft = money(rp, {"structure_area_sqft", "STRUCTAREA", "BLDG_AREA", "GROSS_AREA", "LIVING_AREA"});
        if (row.structure_area_sqft <= 0.0) {
            row.structure_area_sqft = money(&parcel, {"structure_area_sqft", "STRUCTAREA", "BLDG_AREA", "GROSS_AREA", "LIVING_AREA"});
        }
        row.tax_base = money(rp, {"current_value", "tax_base", "TAXBASE", "ARTAXBAS"});
        if (row.tax_base <= 0.0) row.tax_base = money(&parcel, {"current_value", "tax_base", "TAXBASE", "ARTAXBAS"});
        row.sale_price = money(rp, {"sale_price", "SALEPRIC"});
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

const UnifiedParcelRecord* unifiedParcelAt(
    const std::vector<UnifiedParcelRecord>& parcels,
    const std::string& parcel_entity_id) {
    const std::string key = normalizeJoinKey(parcel_entity_id);
    if (key.empty()) return nullptr;
    for (const UnifiedParcelRecord& row : parcels) {
        if (row.parcel_entity_id == key || row.parcel_geometry_entity_id == key) return &row;
    }
    return nullptr;
}

const LayerDef::FeatureRecord* unifiedParcelGeometry(
    const UnifiedParcelRecord& record,
    const std::vector<LayerDef>& layers) {
    if (record.parcel_layer_idx >= layers.size()) return nullptr;
    const auto& layer = layers[record.parcel_layer_idx];
    if (record.parcel_local_feature_idx >= layer.features.size()) return nullptr;
    return &layer.features[record.parcel_local_feature_idx];
}

const LayerDef::FeatureRecord* unifiedRealPropertyGeometry(
    const UnifiedParcelRecord& record,
    const std::vector<LayerDef>& layers) {
    if (record.real_property_layer_idx < 0) return nullptr;
    if ((size_t)record.real_property_layer_idx >= layers.size()) return nullptr;
    if (record.real_property_local_feature_idx == (size_t)-1) return nullptr;
    const auto& layer = layers[(size_t)record.real_property_layer_idx];
    if (record.real_property_local_feature_idx >= layer.features.size()) return nullptr;
    return &layer.features[record.real_property_local_feature_idx];
}

const LayerDef::FeatureRecord* resolveRealPropertyForBlocklot(
    const std::vector<LayerDef>& layers,
    int real_property_layer_idx,
    const std::unordered_map<std::string, size_t>* real_property_by_blocklot,
    const std::string& blocklot) {
    if (real_property_layer_idx < 0) return nullptr;
    if ((size_t)real_property_layer_idx >= layers.size()) return nullptr;
    if (!real_property_by_blocklot) return nullptr;
    if (blocklot.empty()) return nullptr;
    auto it = real_property_by_blocklot->find(blocklot);
    if (it == real_property_by_blocklot->end()) return nullptr;
    const auto& layer = layers[(size_t)real_property_layer_idx];
    if (it->second >= layer.features.size()) return nullptr;
    return &layer.features[it->second];
}
