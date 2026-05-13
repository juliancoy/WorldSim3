#include "parcel_consolidation.h"

#include "app_utils.h"
#include "cache_io.h"
#include "feature_props.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
bool isPropertyOnlySupplementalLayer(const LayerDef& layer) {
    return layer.import_type == "socrata_csv_properties";
}

std::vector<LayerDef::FeatureGeom> loadPropertyOnlyFeaturesFromGeoJson(const fs::path& path) {
    std::vector<LayerDef::FeatureGeom> features;
    std::ifstream in(path);
    if (!in) return features;
    json source;
    in >> source;
    if (!source.contains("features") || !source["features"].is_array()) return features;
    features.reserve(source["features"].size());
    for (const auto& feature : source["features"]) {
        if (!feature.contains("properties") || !feature["properties"].is_object()) continue;
        LayerDef::FeatureGeom fg{};
        for (auto it = feature["properties"].begin(); it != feature["properties"].end(); ++it) {
            if (it.value().is_null()) continue;
            if (it.value().is_string()) fg.properties.push_back({it.key(), it.value().get<std::string>()});
            else fg.properties.push_back({it.key(), it.value().dump()});
        }
        if (!fg.properties.empty()) features.push_back(std::move(fg));
    }
    return features;
}

std::vector<fs::path> supplementalPropertyLayerPaths(const fs::path& root, const std::vector<LayerDef>& layers) {
    std::vector<fs::path> paths;
    for (const auto& layer : layers) {
        if (!isPropertyOnlySupplementalLayer(layer)) continue;
        const fs::path path = root / "data" / "layers" / layer.file;
        if (fs::exists(path)) paths.push_back(path);
    }
    return paths;
}

void accumulateBlocklotCounts(
    const std::vector<LayerDef>& layers,
    int layer_idx,
    bool use_feature_blocklot_key,
    const char* amount_field_primary,
    const char* amount_field_fallback_1,
    const char* amount_field_fallback_2,
    std::unordered_map<std::string, int>& count_by_blocklot,
    std::unordered_map<std::string, double>* amount_by_blocklot) {
    if (layer_idx < 0 || (size_t)layer_idx >= layers.size()) return;
    const auto& features = layers[(size_t)layer_idx].features;
    for (const auto& fg : features) {
        const std::string blocklot = use_feature_blocklot_key
            ? featureBlockLotJoinKey(fg)
            : normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
        if (blocklot.empty()) continue;
        count_by_blocklot[blocklot] += 1;
        if (!amount_by_blocklot) continue;
        double amount = parseNumericField(getPropertyValue(fg, amount_field_primary));
        if (amount <= 0.0 && amount_field_fallback_1) amount = parseNumericField(getPropertyValue(fg, amount_field_fallback_1));
        if (amount <= 0.0 && amount_field_fallback_2) amount = parseNumericField(getPropertyValue(fg, amount_field_fallback_2));
        (*amount_by_blocklot)[blocklot] += amount;
    }
}
}

WorldsimLayerIndices detectWorldsimLayerIndices(
    const fs::path& root,
    const std::vector<LayerDef>& layers) {
    WorldsimLayerIndices indices;
    const bool regional_parcels_available = fs::exists(root / "data" / "layers" / "regional_parcels.geojson");
    const bool regional_real_property_available = fs::exists(root / "data" / "layers" / "regional_real_property.geojson");
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].file == "regional_parcels.geojson" && regional_parcels_available) indices.parcel_layer_idx = (int)i;
        else if (layers[i].file == "parcel.geojson" && indices.parcel_layer_idx < 0) indices.parcel_layer_idx = (int)i;
        else if (layers[i].file == "regional_real_property.geojson" && regional_real_property_available) indices.real_property_layer_idx = (int)i;
        else if (layers[i].file == "real_property_information.geojson" && indices.real_property_layer_idx < 0) {
            indices.real_property_layer_idx = (int)i;
        } else if (layers[i].file == "vacant_building_notices.geojson") {
            indices.vacant_notice_layer_idx = (int)i;
        } else if (layers[i].file == "vacant_building_rehabs.geojson") {
            indices.vacant_rehab_layer_idx = (int)i;
        } else if (layers[i].file == "tax_lien_certificate_sale_properties.geojson") {
            indices.tax_lien_layer_idx = (int)i;
        } else if (layers[i].file == "tax_sale_list_2021.geojson") {
            indices.tax_sale_layer_idx = (int)i;
        } else if (layers[i].file == "zoning.geojson") {
            indices.zoning_layer_idx = (int)i;
        } else if (layers[i].file == "crime_nibrs_group_a_2022_present.geojson") {
            indices.crime_nibrs_layer_idx = (int)i;
        } else if (layers[i].file == "crime_part_1_legacy_srs.geojson") {
            indices.crime_legacy_layer_idx = (int)i;
        }
    }
    return indices;
}

std::string computeHarmonizedRealPropertySignature(
    const fs::path& root,
    const std::vector<LayerDef>& layers,
    int real_property_layer_idx) {
    std::string signature = "rp:";
    if (real_property_layer_idx >= 0 && (size_t)real_property_layer_idx < layers.size()) {
        signature += std::to_string(layers[(size_t)real_property_layer_idx].features.size());
    } else {
        signature += "none";
    }
    for (const auto& path : supplementalPropertyLayerPaths(root, layers)) {
        signature += "|supp:";
        signature += path.filename().string();
        signature += ":";
        signature += fileSignature(path);
    }
    return signature;
}

void rebuildHarmonizedRealPropertyFeatures(
    const fs::path& root,
    const std::vector<LayerDef>& layers,
    int real_property_layer_idx,
    std::vector<LayerDef::FeatureGeom>& harmonized_features,
    std::vector<std::string>& harmonized_source_files,
    std::unordered_map<std::string, size_t>& real_property_by_blocklot) {
    harmonized_features.clear();
    harmonized_source_files.clear();
    real_property_by_blocklot.clear();

    if (real_property_layer_idx >= 0 && (size_t)real_property_layer_idx < layers.size()) {
        const auto& real_property_features = layers[(size_t)real_property_layer_idx].features;
        harmonized_features.insert(harmonized_features.end(), real_property_features.begin(), real_property_features.end());
        harmonized_source_files.insert(
            harmonized_source_files.end(),
            real_property_features.size(),
            layers[(size_t)real_property_layer_idx].file);
    }
    for (const auto& layer : layers) {
        if (!isPropertyOnlySupplementalLayer(layer)) continue;
        const fs::path path = root / "data" / "layers" / layer.file;
        if (!fs::exists(path)) continue;
        auto extra_features = loadPropertyOnlyFeaturesFromGeoJson(path);
        harmonized_source_files.insert(harmonized_source_files.end(), extra_features.size(), layer.file);
        harmonized_features.insert(
            harmonized_features.end(),
            std::make_move_iterator(extra_features.begin()),
            std::make_move_iterator(extra_features.end()));
    }
    for (size_t i = 0; i < harmonized_features.size(); ++i) {
        const std::string blocklot = featureBlockLotJoinKey(harmonized_features[i]);
        if (!blocklot.empty() && !real_property_by_blocklot.contains(blocklot)) {
            real_property_by_blocklot[blocklot] = i;
        }
    }
}

ParcelConsolidationArtifacts buildParcelConsolidationArtifacts(
    const fs::path& root,
    const std::vector<LayerDef>& layers,
    const WorldsimLayerIndices& layer_indices) {
    ParcelConsolidationArtifacts artifacts;
    rebuildHarmonizedRealPropertyFeatures(
        root,
        layers,
        layer_indices.real_property_layer_idx,
        artifacts.harmonized_real_property_features,
        artifacts.harmonized_real_property_source_files,
        artifacts.real_property_by_blocklot);

    std::unordered_map<std::string, int> vacant_notice_count_by_blocklot;
    std::unordered_map<std::string, int> vacant_rehab_count_by_blocklot;
    std::unordered_map<std::string, int> tax_lien_count_by_blocklot;
    std::unordered_map<std::string, int> tax_sale_count_by_blocklot;
    std::unordered_map<std::string, double> tax_lien_amount_by_blocklot;
    std::unordered_map<std::string, double> tax_sale_amount_by_blocklot;

    accumulateBlocklotCounts(
        layers,
        layer_indices.vacant_notice_layer_idx,
        false,
        "",
        nullptr,
        nullptr,
        vacant_notice_count_by_blocklot,
        nullptr);
    accumulateBlocklotCounts(
        layers,
        layer_indices.vacant_rehab_layer_idx,
        false,
        "",
        nullptr,
        nullptr,
        vacant_rehab_count_by_blocklot,
        nullptr);
    accumulateBlocklotCounts(
        layers,
        layer_indices.tax_lien_layer_idx,
        true,
        "TOTAL_AMOUNT",
        nullptr,
        nullptr,
        tax_lien_count_by_blocklot,
        &tax_lien_amount_by_blocklot);
    accumulateBlocklotCounts(
        layers,
        layer_indices.tax_sale_layer_idx,
        true,
        "total_lien",
        "total_3yea",
        "total_tax",
        tax_sale_count_by_blocklot,
        &tax_sale_amount_by_blocklot);

    if (layer_indices.parcel_layer_idx < 0 || (size_t)layer_indices.parcel_layer_idx >= layers.size()) {
        return artifacts;
    }

    const auto& parcel_features = layers[(size_t)layer_indices.parcel_layer_idx].features;
    artifacts.parcel_vac_notice_by_feature.assign(parcel_features.size(), 0);
    artifacts.parcel_vac_rehab_by_feature.assign(parcel_features.size(), 0);
    artifacts.parcel_tax_lien_by_feature.assign(parcel_features.size(), 0);
    artifacts.parcel_tax_sale_by_feature.assign(parcel_features.size(), 0);
    artifacts.parcel_tax_lien_amount_by_feature.assign(parcel_features.size(), 0.0);
    artifacts.parcel_tax_sale_amount_by_feature.assign(parcel_features.size(), 0.0);

    for (size_t i = 0; i < parcel_features.size(); ++i) {
        const std::string normalized_blocklot = normalizeJoinKey(getPropertyValue(parcel_features[i], "BLOCKLOT"));
        auto notice_it = vacant_notice_count_by_blocklot.find(normalized_blocklot);
        if (notice_it != vacant_notice_count_by_blocklot.end()) {
            artifacts.parcel_vac_notice_by_feature[i] = notice_it->second;
        }
        auto rehab_it = vacant_rehab_count_by_blocklot.find(normalized_blocklot);
        if (rehab_it != vacant_rehab_count_by_blocklot.end()) {
            artifacts.parcel_vac_rehab_by_feature[i] = rehab_it->second;
        }

        const std::string blocklot = featureBlockLotJoinKey(parcel_features[i]);
        auto lien_it = tax_lien_count_by_blocklot.find(blocklot);
        if (lien_it != tax_lien_count_by_blocklot.end()) {
            artifacts.parcel_tax_lien_by_feature[i] = lien_it->second;
            auto amount_it = tax_lien_amount_by_blocklot.find(blocklot);
            if (amount_it != tax_lien_amount_by_blocklot.end()) {
                artifacts.parcel_tax_lien_amount_by_feature[i] = amount_it->second;
            }
        }
        auto sale_it = tax_sale_count_by_blocklot.find(blocklot);
        if (sale_it != tax_sale_count_by_blocklot.end()) {
            artifacts.parcel_tax_sale_by_feature[i] = sale_it->second;
            auto amount_it = tax_sale_amount_by_blocklot.find(blocklot);
            if (amount_it != tax_sale_amount_by_blocklot.end()) {
                artifacts.parcel_tax_sale_amount_by_feature[i] = amount_it->second;
            }
        }
    }

    artifacts.unified_parcels = buildUnifiedParcels(UnifiedParcelBuildRequest{
        &layers,
        layer_indices.parcel_layer_idx,
        layer_indices.real_property_layer_idx,
        &artifacts.harmonized_real_property_features,
        &artifacts.harmonized_real_property_source_files,
        &artifacts.real_property_by_blocklot,
        &artifacts.parcel_vac_notice_by_feature,
        &artifacts.parcel_vac_rehab_by_feature,
        &artifacts.parcel_tax_lien_by_feature,
        &artifacts.parcel_tax_sale_by_feature,
        &artifacts.parcel_tax_lien_amount_by_feature,
        &artifacts.parcel_tax_sale_amount_by_feature,
    });

    return artifacts;
}
