#pragma once

#include "owners_tab.h"
#include "parcel_unified.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct OwnerAggregatesContext {
    const std::filesystem::path* root = nullptr;
    const std::vector<LayerDef>* layers = nullptr;
    const std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;

    int parcel_layer_idx = -1;
    int real_property_layer_idx = -1;
    int parcel_vacancy_generation_applied = 0;
    int parcel_tax_generation_applied = 0;

    std::unordered_set<std::string>* selected_owners = nullptr;
    std::unordered_map<std::string, std::string>* owner_class_overrides = nullptr;
    bool* owner_class_overrides_loaded = nullptr;
    bool* owner_class_overrides_dirty = nullptr;

    std::vector<OwnerAggregate>* owner_aggregates = nullptr;
    FilteredAggregateSnapshot* filtered_aggregate_snapshot = nullptr;
    bool* owner_aggregates_dirty = nullptr;
    int* owner_sorted_mode = nullptr;
    size_t* owner_cached_parcel_size = nullptr;
    size_t* owner_cached_real_property_size = nullptr;

    std::atomic<double>* prof_owner_ms_last = nullptr;
};

const std::vector<std::pair<std::string, std::string>>& ownerClassItems();
void syncOwnerAggregates(const OwnerAggregatesContext& ctx);
