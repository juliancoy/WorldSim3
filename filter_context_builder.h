#pragma once

#include "filters.h"

struct FeatureFilterContextFactoryInput {
    const std::vector<LayerDef>* layers = nullptr;
    const MapFilterState* map_filters = nullptr;
    const FilterResultSet* result_set = nullptr;
    const std::vector<QueryMapLayer>* query_layers = nullptr;
    const std::unordered_map<std::string, size_t>* real_property_by_blocklot = nullptr;
    const std::vector<int>* parcel_vac_notice_by_feature = nullptr;
    const std::vector<int>* parcel_vac_rehab_by_feature = nullptr;
    int real_property_layer_idx = -1;
    int parcel_layer_idx = -1;
    int crime_nibrs_layer_idx = -1;
};

FeatureFilterContext makeFeatureFilterContext(const FeatureFilterContextFactoryInput& input);
