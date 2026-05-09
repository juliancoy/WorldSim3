#pragma once

#include "types.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct FeatureFilterContext {
    const std::vector<LayerDef>* layers = nullptr;
    const std::unordered_map<std::string, size_t>* real_property_by_blocklot = nullptr;
    const std::unordered_set<std::string>* selected_owners = nullptr;
    const std::vector<int>* parcel_vac_notice_by_feature = nullptr;
    const std::vector<int>* parcel_vac_rehab_by_feature = nullptr;

    int real_property_layer_idx = -1;
    int parcel_layer_idx = -1;
    int crime_nibrs_layer_idx = -1;
    int crime_legacy_layer_idx = -1;

    bool filter_enabled = false;
    bool filter_use_date = false;
    int filter_year_min = 1900;
    int filter_year_max = 2100;
    std::string filter_blocklot;
    std::string filter_status;
    std::string filter_address;
    std::string filter_owner;
    std::string filter_zip;

    bool crime_filter_enabled = false;
    bool crime_filter_homicide = false;
    bool crime_filter_robbery = false;
    bool crime_filter_assault = false;
    bool crime_filter_burglary = false;
    bool crime_filter_theft = false;
    bool crime_filter_auto_theft = false;
    bool crime_filter_drug = false;
    bool crime_filter_shooting = false;
    bool crime_filter_use_year = false;
    int crime_year_min = 1900;
    int crime_year_max = 2100;
};

bool isParcelRelatedLayer(const FeatureFilterContext& ctx, size_t layer_idx);
bool featurePassesFilters(
    const FeatureFilterContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureGeom& fg);
