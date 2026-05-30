#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct UnifiedParcelRecord;

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

struct LayerBrowseState {
    std::string selected_nation_state = "us";
    std::string selected_state_region = "md";
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
    std::unordered_map<std::string, bool> event_sector_enabled;
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

struct QueryRecord {
    bool enabled = true;
    std::string executed_at_utc;
    std::string mode;
    std::string name;
    std::string sql;
    float color[4] = {1.0f, 0.48f, 0.08f, 1.0f};
    float outline_color[4] = {1.0f, 0.48f, 0.08f, 1.0f};
    FilterResultSet result_set;
    size_t row_count = 0;
    std::string status;
    struct ExecutionContextSnapshot {
        bool filter_enabled = false;
        bool filter_use_date = false;
        int filter_year_min = 2000;
        int filter_year_max = 2026;
        std::string filter_blocklot;
        std::string filter_status;
        std::string filter_address;
        std::string filter_owner;
        std::string filter_zip;
        CrimeFilterState crime;
        std::vector<std::string> selected_owners;
        std::vector<std::string> selected_parcel_blocklots;
        std::unordered_map<std::string, bool> event_sector_enabled;
        double center_lon = -76.6122;
        double center_lat = 39.2904;
        double zoom = 12.0;
        std::string map_title_text;
        bool map_title_show_primary_parcel_source = false;
    } snapshot;
};

using QueryExecutionContextSnapshot = QueryRecord::ExecutionContextSnapshot;
using QueryMapLayer = QueryRecord;
using QueryHistoryEntry = QueryRecord;

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
    std::string owner_filter_normalized;
    double* owner_filter_ms_accum = nullptr;
    size_t* owner_filter_candidates_accum = nullptr;
    size_t* owner_filter_matches_accum = nullptr;
    bool compiled_owner_filter_active = false;
    bool compiled_address_filter_active = false;

    int real_property_layer_idx = -1;
    int parcel_layer_idx = -1;
    int crime_nibrs_layer_idx = -1;
};

struct FeatureRenderState {
    bool visible = true;
    bool has_query_color = false;
    ImU32 query_color = IM_COL32(0, 0, 0, 0);
    bool has_query_outline_color = false;
    ImU32 query_outline_color = IM_COL32(0, 0, 0, 0);
};

struct LayerFeatureRenderCache {
    uint64_t state_key = 0;
    std::vector<std::vector<FeatureRenderState>> layer_states;
};

struct FeatureRenderStateKeyContext {
    const MapFilterState* map_filters = nullptr;
    const FilterResultSet* result_set = nullptr;
    const FilterResultSet* secondary_result_set = nullptr;
    const FilterResultSet* tertiary_result_set = nullptr;
    const std::vector<QueryMapLayer>* query_layers = nullptr;
};

bool isParcelRelatedLayer(const FeatureFilterContext& ctx, size_t layer_idx);
bool featurePassesFilters(
    const FeatureFilterContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg);

bool queryMapColorForFeature(
    const FeatureFilterContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg,
    float out_color[4]);

bool queryMapStyleForFeature(
    const FeatureFilterContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg,
    float out_color[4],
    float out_outline_color[4]);

uint64_t buildFeatureRenderStateKey(const FeatureRenderStateKeyContext& ctx);

bool ensureLayerFeatureRenderCache(
    const FeatureFilterContext& ctx,
    const std::vector<LayerDef>& layers,
    uint64_t state_key,
    LayerFeatureRenderCache& cache);

const FeatureRenderState* findFeatureRenderState(
    const LayerFeatureRenderCache& cache,
    size_t layer_idx,
    size_t feature_idx);

bool layerMatchesBrowseGeography(const LayerDef& layer, const LayerBrowseState& browse_state);
