#include "parcel_consolidation.h"

#include "app_utils.h"
#include "cache_io.h"
#include "feature_props.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
bool containsCaseInsensitive(const std::string& haystack, const char* needle) {
    if (!needle || !*needle) return false;
    std::string hs = haystack;
    std::string nd = needle;
    std::transform(hs.begin(), hs.end(), hs.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(nd.begin(), nd.end(), nd.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return hs.find(nd) != std::string::npos;
}

int zoningLayerMatchScore(const LayerDef& layer) {
    const bool non_point = !layerUsesPointGeometry(layer);
    if (layer.category == LayerDef::Category::Zoning && non_point) return 4;
    if (layer.file == "zoning.geojson") return 3;
    if (non_point && containsCaseInsensitive(layer.file, "zoning")) return 2;
    if (non_point && containsCaseInsensitive(layer.name, "zoning")) return 1;
    return 0;
}

bool layerFileMaterialized(const fs::path& root, const std::string& file) {
    return layerRuntimeSourceMaterializedForFile(root, file);
}

bool isDirectOperationalParcelLayer(const LayerDef& layer) {
    return layer.scale == "parcel" &&
           layer.duckdb_role == "parcel_record";
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
    const bool regional_real_property_available =
        layerRuntimeSourceMaterializedForFile(root, "regional_real_property.geojson");
    int first_enabled_materialized_parcel_idx = -1;
    int first_materialized_parcel_idx = -1;
    int first_enabled_city_parcel_idx = -1;
    int first_city_parcel_idx = -1;
    int best_zoning_match = 0;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (isDirectOperationalParcelLayer(layers[i]) &&
            layerFileMaterialized(root, layers[i].file)) {
            if (first_materialized_parcel_idx < 0) first_materialized_parcel_idx = (int)i;
            if (layers[i].enabled && first_enabled_materialized_parcel_idx < 0) {
                first_enabled_materialized_parcel_idx = (int)i;
            }
        }
        if (layers[i].file == "parcel.geojson") {
            if (first_city_parcel_idx < 0) first_city_parcel_idx = (int)i;
            if (layers[i].enabled && first_enabled_city_parcel_idx < 0) {
                first_enabled_city_parcel_idx = (int)i;
            }
        }
        if (layers[i].file == "regional_real_property.geojson" && regional_real_property_available) indices.real_property_layer_idx = (int)i;
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
        } else if (layers[i].file == "crime_nibrs_group_a_2022_present.geojson") {
            indices.crime_nibrs_layer_idx = (int)i;
        }
        const int zoning_match = zoningLayerMatchScore(layers[i]);
        if (zoning_match > best_zoning_match) {
            best_zoning_match = zoning_match;
            indices.zoning_layer_idx = (int)i;
        }
    }
    if (first_enabled_materialized_parcel_idx >= 0) {
        indices.parcel_layer_idx = first_enabled_materialized_parcel_idx;
    } else if (first_enabled_city_parcel_idx >= 0) {
        indices.parcel_layer_idx = first_enabled_city_parcel_idx;
    } else if (first_materialized_parcel_idx >= 0) {
        indices.parcel_layer_idx = first_materialized_parcel_idx;
    } else if (first_city_parcel_idx >= 0) {
        indices.parcel_layer_idx = first_city_parcel_idx;
    }
    return indices;
}

std::string computeHarmonizedRealPropertySignature(
    const fs::path& root,
    const std::vector<LayerDef>& layers,
    int real_property_layer_idx) {
    (void)root;
    (void)layers;
    std::string signature = "rp:";
    if (real_property_layer_idx >= 0 && (size_t)real_property_layer_idx < layers.size()) {
        signature += std::to_string(layers[(size_t)real_property_layer_idx].features.size());
    } else {
        signature += "none";
    }
    return signature;
}

void rebuildHarmonizedRealPropertyFeatures(
    const fs::path& root,
    const std::vector<LayerDef>& layers,
    int real_property_layer_idx,
    std::vector<LayerDef::FeatureRecord>& harmonized_features,
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

    std::vector<size_t> parcel_layer_indices;
    for (size_t li = 0; li < layers.size(); ++li) {
        if (!isDirectOperationalParcelLayer(layers[li])) continue;
        if (layers[li].features.empty()) continue;
        parcel_layer_indices.push_back(li);
    }
    if (parcel_layer_indices.empty() &&
        layer_indices.parcel_layer_idx >= 0 &&
        (size_t)layer_indices.parcel_layer_idx < layers.size()) {
        parcel_layer_indices.push_back((size_t)layer_indices.parcel_layer_idx);
    }
    std::sort(parcel_layer_indices.begin(), parcel_layer_indices.end());
    parcel_layer_indices.erase(
        std::unique(parcel_layer_indices.begin(), parcel_layer_indices.end()),
        parcel_layer_indices.end());
    if (parcel_layer_indices.empty()) {
        return artifacts;
    }

    if (layer_indices.parcel_layer_idx >= 0 &&
        (size_t)layer_indices.parcel_layer_idx < layers.size()) {
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
    }

    size_t unified_reserve = 0;
    for (const size_t parcel_layer_idx : parcel_layer_indices) {
        unified_reserve += layers[parcel_layer_idx].features.size();
    }
    artifacts.unified_parcels.reserve(unified_reserve);
    for (const size_t parcel_layer_idx : parcel_layer_indices) {
        const bool active_parcel_layer =
            layer_indices.parcel_layer_idx >= 0 &&
            parcel_layer_idx == (size_t)layer_indices.parcel_layer_idx;
        std::vector<UnifiedParcelRecord> layer_unified = buildUnifiedParcels(UnifiedParcelBuildRequest{
            &layers,
            nullptr,
            (int)parcel_layer_idx,
            layer_indices.real_property_layer_idx,
            &artifacts.harmonized_real_property_features,
            &artifacts.harmonized_real_property_source_files,
            &artifacts.real_property_by_blocklot,
            active_parcel_layer ? &artifacts.parcel_vac_notice_by_feature : nullptr,
            active_parcel_layer ? &artifacts.parcel_vac_rehab_by_feature : nullptr,
            active_parcel_layer ? &artifacts.parcel_tax_lien_by_feature : nullptr,
            active_parcel_layer ? &artifacts.parcel_tax_sale_by_feature : nullptr,
            active_parcel_layer ? &artifacts.parcel_tax_lien_amount_by_feature : nullptr,
            active_parcel_layer ? &artifacts.parcel_tax_sale_amount_by_feature : nullptr,
        });
        artifacts.unified_parcels.insert(
            artifacts.unified_parcels.end(),
            std::make_move_iterator(layer_unified.begin()),
            std::make_move_iterator(layer_unified.end()));
    }

    return artifacts;
}
