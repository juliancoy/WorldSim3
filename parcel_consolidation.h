#pragma once

#include "layer_registry.h"
#include "parcel_unified.h"
#include "types.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct ParcelConsolidationArtifacts {
    std::unordered_map<std::string, size_t> real_property_by_blocklot;
    std::vector<LayerDef::FeatureGeom> harmonized_real_property_features;
    std::vector<std::string> harmonized_real_property_source_files;
    std::vector<int> parcel_vac_notice_by_feature;
    std::vector<int> parcel_vac_rehab_by_feature;
    std::vector<int> parcel_tax_lien_by_feature;
    std::vector<int> parcel_tax_sale_by_feature;
    std::vector<double> parcel_tax_lien_amount_by_feature;
    std::vector<double> parcel_tax_sale_amount_by_feature;
    std::vector<UnifiedParcelRecord> unified_parcels;
};

WorldsimLayerIndices detectWorldsimLayerIndices(
    const std::filesystem::path& root,
    const std::vector<LayerDef>& layers);

ParcelConsolidationArtifacts buildParcelConsolidationArtifacts(
    const std::filesystem::path& root,
    const std::vector<LayerDef>& layers,
    const WorldsimLayerIndices& layer_indices);

std::string computeHarmonizedRealPropertySignature(
    const std::filesystem::path& root,
    const std::vector<LayerDef>& layers,
    int real_property_layer_idx);

void rebuildHarmonizedRealPropertyFeatures(
    const std::filesystem::path& root,
    const std::vector<LayerDef>& layers,
    int real_property_layer_idx,
    std::vector<LayerDef::FeatureGeom>& harmonized_features,
    std::vector<std::string>& harmonized_source_files,
    std::unordered_map<std::string, size_t>& real_property_by_blocklot);
