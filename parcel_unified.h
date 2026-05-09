#pragma once

#include "types.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct UnifiedParcelRecord {
    size_t parcel_layer_idx = 0;
    size_t parcel_feature_idx = 0;
    std::string blocklot;

    const LayerDef::FeatureGeom* parcel_geom = nullptr;
    const LayerDef::FeatureGeom* real_property = nullptr;
    size_t real_property_feature_idx = (size_t)-1;

    std::string owner;
    std::string owner_display;
    std::string address;
    std::string zip;
    std::string status;

    double current_land = 0.0;
    double current_improvements = 0.0;
    double tax_base = 0.0;
    double sale_price = 0.0;
    double current_value = 0.0;

    int vacant_notice_count = 0;
    int vacant_rehab_count = 0;
    int tax_lien_count = 0;
    int tax_sale_count = 0;
    double tax_lien_amount = 0.0;
    double tax_sale_amount = 0.0;
};

struct UnifiedParcelBuildRequest {
    const std::vector<LayerDef>* layers = nullptr;
    int parcel_layer_idx = -1;
    int real_property_layer_idx = -1;
    const std::unordered_map<std::string, size_t>* real_property_by_blocklot = nullptr;
    const std::vector<int>* parcel_vac_notice_by_feature = nullptr;
    const std::vector<int>* parcel_vac_rehab_by_feature = nullptr;
    const std::vector<int>* parcel_tax_lien_by_feature = nullptr;
    const std::vector<int>* parcel_tax_sale_by_feature = nullptr;
    const std::vector<double>* parcel_tax_lien_amount_by_feature = nullptr;
    const std::vector<double>* parcel_tax_sale_amount_by_feature = nullptr;
};

std::vector<UnifiedParcelRecord> buildUnifiedParcels(const UnifiedParcelBuildRequest& request);

const UnifiedParcelRecord* unifiedParcelAt(const std::vector<UnifiedParcelRecord>& parcels, size_t parcel_feature_idx);
