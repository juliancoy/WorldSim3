#pragma once

#include "cache_io.h"
#include "types.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct UnifiedParcelRecord {
    size_t parcel_layer_idx = 0;
    size_t parcel_feature_idx = 0;
    std::string blocklot;

    int real_property_layer_idx = -1;
    size_t real_property_feature_idx = (size_t)-1;
    std::string parcel_source_file;
    std::string property_source_file;
    bool parcel_has_geometry = false;
    LayerDef::FeatureExtent parcel_extent;
    bool has_property_record = false;

    std::string owner;
    std::string owner_display;
    std::string owner_search;
    std::string address;
    std::string address_search;
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
    const ParcelRenderCacheBlob* parcel_render_blob = nullptr;
    int parcel_layer_idx = -1;
    int real_property_layer_idx = -1;
    const std::vector<LayerDef::FeatureRecord>* real_property_features = nullptr;
    const std::vector<std::string>* real_property_source_files = nullptr;
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
const LayerDef::FeatureRecord* unifiedParcelGeometry(
    const UnifiedParcelRecord& record,
    const std::vector<LayerDef>& layers);
const LayerDef::FeatureRecord* unifiedRealPropertyGeometry(
    const UnifiedParcelRecord& record,
    const std::vector<LayerDef>& layers);
const LayerDef::FeatureRecord* resolveRealPropertyForBlocklot(
    const std::vector<LayerDef>& layers,
    int real_property_layer_idx,
    const std::unordered_map<std::string, size_t>* real_property_by_blocklot,
    const std::string& blocklot);
