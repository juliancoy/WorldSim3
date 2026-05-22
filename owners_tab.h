#pragma once

#include "owner_info.h"
#include "types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct OwnerAggregate {
    std::string owner;
    std::string owner_class;
    size_t property_count = 0;
    double area_m2 = 0.0;
    double value_usd = 0.0;
};

struct FilteredAggregateSnapshot {
    bool valid = false;
    uint64_t selection_key = 0;
    uint64_t data_key = 0;
    size_t vacancy_parcels_matched = 0;
    size_t vacancy_parcels_with_geometry = 0;
    size_t owner_property_count = 0;
    double owner_area_m2 = 0.0;
    double owner_value_usd = 0.0;
};

struct OwnersTabContext {
    std::vector<OwnerAggregate>* owner_aggregates = nullptr;
    std::unordered_set<std::string>* selected_owners = nullptr;
    std::unordered_map<std::string, std::string>* owner_class_overrides = nullptr;
    const FilteredAggregateSnapshot* filtered_aggregate_snapshot = nullptr;
    OwnerInfoUiState* owner_info_state = nullptr;
    const std::vector<LayerDef>* layers = nullptr;
    int parcel_layer_idx = -1;
    int real_property_layer_idx = -1;
    int* owner_sort_mode = nullptr;
    int* owner_sorted_mode = nullptr;
    int* owner_class_filter_mode = nullptr;
    int* owner_class_assign_mode = nullptr;
    int* selected_owner_anchor = nullptr;
    bool* owner_class_overrides_dirty = nullptr;
    bool* owner_aggregates_dirty = nullptr;
    char* owner_search_query = nullptr;
    size_t owner_search_query_size = 0;
    const std::vector<std::pair<std::string, std::string>>* owner_class_items = nullptr;
    std::function<void()> sync_owner_aggregates;
};

void drawOwnersTab(const OwnersTabContext& ctx);
