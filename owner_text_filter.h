#pragma once

#include "filters.h"
#include "parcel_unified.h"

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <string>
#include <vector>

struct OwnerTextSearchIndex {
    std::unordered_map<uint32_t, std::vector<uint32_t>> trigram_postings;
};

struct OwnerTextFilterState {
    FilterResultSet result_set;
    std::string cached_query;
    std::string cached_parcel_signature;
    std::string cached_real_property_signature;
    size_t cached_parcel_count = 0;
    size_t cached_real_property_count = 0;
    OwnerTextSearchIndex search_index;
};

struct OwnerTextFilterRefreshContext {
    const MapFilterState* map_filters = nullptr;
    const std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;
    const std::vector<std::string>* parcel_owner_search_by_feature = nullptr;
    int parcel_layer_idx = -1;
    int real_property_layer_idx = -1;
    const std::string* parcel_signature = nullptr;
    const std::string* real_property_signature = nullptr;
    size_t real_property_count = 0;
};

void refreshOwnerTextFilterState(
    OwnerTextFilterState& state,
    const OwnerTextFilterRefreshContext& ctx);
