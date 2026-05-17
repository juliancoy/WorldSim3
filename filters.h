#pragma once

#include "types.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct CrimeFilterState {
    bool enabled = false;
    bool homicide = false;
    bool robbery = false;
    bool assault = false;
    bool burglary = false;
    bool theft = false;
    bool auto_theft = false;
    bool drug = false;
    bool shooting = false;
    bool use_year = false;
    int year_min = 2022;
    int year_max = 2026;
};

struct MapFilterState {
    // SSOT for map filters created by UI controls. Rendering reads this via
    // FeatureFilterContext; individual tabs should mutate only this object.
    bool enabled = false;
    bool use_date = false;
    int year_min = 2000;
    int year_max = 2026;
    char blocklot[64] = "";
    char status[64] = "";
    char address[160] = "";
    char owner[96] = "";
    char zip[24] = "";
    CrimeFilterState crime;
    std::unordered_set<std::string> selected_owners;
};

struct FeatureKey {
    size_t layer_idx = 0;
    size_t feature_idx = 0;

    bool operator==(const FeatureKey& other) const {
        return layer_idx == other.layer_idx && feature_idx == other.feature_idx;
    }
};

struct FeatureKeyHash {
    size_t operator()(const FeatureKey& key) const {
        size_t h = key.layer_idx + 0x9e3779b97f4a7c15ULL;
        h ^= key.feature_idx + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

struct FilterResultSet {
    // For filters that are produced outside the immediate UI, e.g. SQL query
    // results. These sets are canonical render-domain outputs, not UI state.
    bool active = false;
    std::unordered_set<size_t> layers;
    std::unordered_set<FeatureKey, FeatureKeyHash> features;
    std::unordered_set<std::string> blocklots;
    std::unordered_set<std::string> owners;
};

struct QueryMapLayer {
    bool enabled = true;
    std::string name;
    std::string sql;
    float color[4] = {1.0f, 0.48f, 0.08f, 1.0f};
    FilterResultSet result_set;
    size_t row_count = 0;
    std::string status;
};

struct ParcelJurisdictionFilterState {
    std::unordered_set<std::string> selected_jurisdictions;
    bool dirty = true;
    FilterResultSet result_set;
    std::string status = "All Maryland parcels";
};

struct FeatureFilterContext {
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

bool isParcelRelatedLayer(const FeatureFilterContext& ctx, size_t layer_idx);
bool featurePassesFilters(
    const FeatureFilterContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureGeom& fg);

bool queryMapColorForFeature(
    const FeatureFilterContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureGeom& fg,
    float out_color[4]);
