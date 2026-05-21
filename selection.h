#pragma once

#include "types.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

struct ParcelSelectionState {
    bool show_details = false;
    size_t active_idx = (size_t)-1;
    std::string active_stable_id;
    std::vector<size_t> indices;
    std::unordered_set<size_t> index_set;
    std::vector<std::string> stable_ids;
    std::unordered_set<std::string> stable_id_set;
};

void clearParcelSelection(ParcelSelectionState& selection);
bool selectParcel(
    ParcelSelectionState& selection,
    size_t idx,
    const std::string& stable_id,
    size_t parcel_count,
    bool append_toggle);
bool selectParcel(ParcelSelectionState& selection, size_t idx, size_t parcel_count, bool append_toggle);
void pruneParcelSelection(ParcelSelectionState& selection, size_t parcel_count);
void reconcileParcelSelection(ParcelSelectionState& selection, const LayerDef& parcel_layer);
