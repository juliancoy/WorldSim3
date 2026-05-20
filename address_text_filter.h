#pragma once

#include "filters.h"
#include "parcel_unified.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct AddressTextSearchIndex {
    std::unordered_map<uint32_t, std::vector<uint32_t>> trigram_postings;
};

struct AddressTextFilterState {
    FilterResultSet result_set;
    std::string cached_query;
    std::string cached_parcel_signature;
    size_t cached_parcel_count = 0;
    AddressTextSearchIndex search_index;
};

struct AddressTextFilterRefreshContext {
    const MapFilterState* map_filters = nullptr;
    const std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;
    const std::vector<std::string>* parcel_address_search_by_feature = nullptr;
    int parcel_layer_idx = -1;
    int real_property_layer_idx = -1;
    const std::string* parcel_signature = nullptr;
};

void refreshAddressTextFilterState(
    AddressTextFilterState& state,
    const AddressTextFilterRefreshContext& ctx);
