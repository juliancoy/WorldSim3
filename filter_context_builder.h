#pragma once

#include "filters.h"

struct FeatureFilterContextFactoryInput {
    const std::vector<LayerDef>* layers = nullptr;
    const MapFilterState* map_filters = nullptr;
    const FilterResultSet* result_set = nullptr;
    const FilterResultSet* secondary_result_set = nullptr;
    const FilterResultSet* tertiary_result_set = nullptr;
    const std::vector<QueryMapLayer>* query_layers = nullptr;
    const std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;
    const std::vector<std::string>* parcel_owner_search_by_feature = nullptr;
    const std::vector<std::string>* real_property_owner_search_by_feature = nullptr;
    const std::vector<std::string>* parcel_address_search_by_feature = nullptr;
    const std::unordered_map<std::string, size_t>* real_property_by_blocklot = nullptr;
    const std::vector<int>* parcel_vac_notice_by_feature = nullptr;
    const std::vector<int>* parcel_vac_rehab_by_feature = nullptr;
    int real_property_layer_idx = -1;
    int parcel_layer_idx = -1;
    int crime_nibrs_layer_idx = -1;
    double* owner_filter_ms_accum = nullptr;
    size_t* owner_filter_candidates_accum = nullptr;
    size_t* owner_filter_matches_accum = nullptr;
    bool compiled_owner_filter_active = false;
    bool compiled_address_filter_active = false;
};

FeatureFilterContext makeFeatureFilterContext(const FeatureFilterContextFactoryInput& input);
