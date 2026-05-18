#pragma once

#include "app_settings.h"
#include "layer_runtime.h"
#include "parcel_unified.h"
#include "types.h"
#include "zoning.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct DerivedLayerCachesContext {
    const std::filesystem::path* root = nullptr;
    std::vector<LayerDef>* layers = nullptr;
    const std::vector<LayerRuntimeState>* layer_states = nullptr;
    const AppSettings* app_settings = nullptr;

    int zoning_layer_idx = -1;
    int real_property_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
    int parcel_layer_idx = -1;

    std::unordered_map<std::string, ZoneMetadata>* zoning_metadata = nullptr;
    std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr;
    std::unordered_map<std::string, ImVec4>* zoning_zone_color = nullptr;
    std::unordered_map<std::string, std::string>* zoning_zone_label = nullptr;
    std::vector<std::string>* zoning_zone_order = nullptr;
    std::unordered_map<std::string, size_t>* zoning_zone_counts = nullptr;
    std::unordered_map<std::string, std::vector<std::string>>* zoning_group_zones = nullptr;
    std::vector<std::string>* zoning_group_order = nullptr;
    size_t* zoning_zone_discovered_feature_count = nullptr;

    std::unordered_map<std::string, size_t>* real_property_by_blocklot = nullptr;
    std::vector<LayerDef::FeatureGeom>* harmonized_real_property_features = nullptr;
    std::vector<std::string>* harmonized_real_property_source_files = nullptr;
    std::string* harmonized_real_property_signature = nullptr;
    size_t* cached_real_property_size = nullptr;
    size_t* cached_vac_notice_size = nullptr;
    std::string* cached_vac_notice_signature = nullptr;
    size_t* cached_vac_rehab_size = nullptr;
    std::string* cached_vac_rehab_signature = nullptr;
    size_t* cached_tax_lien_size = nullptr;
    std::string* cached_tax_lien_signature = nullptr;
    size_t* cached_tax_sale_size = nullptr;
    std::string* cached_tax_sale_signature = nullptr;

    std::unordered_map<std::string, int>* vacant_notice_count_by_blocklot = nullptr;
    std::unordered_map<std::string, int>* vacant_rehab_count_by_blocklot = nullptr;
    std::unordered_map<std::string, int>* tax_lien_count_by_blocklot = nullptr;
    std::unordered_map<std::string, double>* tax_lien_amount_by_blocklot = nullptr;
    std::unordered_map<std::string, int>* tax_sale_count_by_blocklot = nullptr;
    std::unordered_map<std::string, double>* tax_sale_amount_by_blocklot = nullptr;

    int* vacancy_maps_generation = nullptr;
    int* parcel_vacancy_generation_applied = nullptr;
    int* tax_maps_generation = nullptr;
    int* parcel_tax_generation_applied = nullptr;

    std::vector<int>* parcel_vac_notice_by_feature = nullptr;
    std::vector<int>* parcel_vac_rehab_by_feature = nullptr;
    std::vector<int>* parcel_tax_lien_by_feature = nullptr;
    std::vector<int>* parcel_tax_sale_by_feature = nullptr;
    std::vector<double>* parcel_tax_lien_amount_by_feature = nullptr;
    std::vector<double>* parcel_tax_sale_amount_by_feature = nullptr;

    std::atomic<size_t>* vacant_notice_rows_matched_total = nullptr;
    std::atomic<size_t>* vacant_rehab_rows_matched_total = nullptr;
    std::atomic<size_t>* vacant_parcels_matched_total = nullptr;
    std::atomic<size_t>* vacant_parcels_with_geometry_total = nullptr;
    std::atomic<size_t>* vacant_parcels_triangulated_renderable_total = nullptr;

    std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;
    size_t* unified_parcel_cached_size = nullptr;
    std::string* unified_parcel_cached_signature = nullptr;
    std::string* last_refresh_inputs_signature = nullptr;
    size_t* unified_real_property_cached_size = nullptr;
    int* unified_vacancy_generation_applied = nullptr;
    int* unified_tax_generation_applied = nullptr;

    bool* owner_aggregates_dirty = nullptr;
};

void refreshDerivedLayerCaches(DerivedLayerCachesContext& ctx);
