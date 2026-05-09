#pragma once

#include <cstddef>
#include <unordered_set>
#include <vector>

struct ParcelSelectionState {
    bool show_details = false;
    size_t active_idx = (size_t)-1;
    std::vector<size_t> indices;
    std::unordered_set<size_t> index_set;
};

void clearParcelSelection(ParcelSelectionState& selection);
bool selectParcel(ParcelSelectionState& selection, size_t idx, size_t parcel_count, bool append_toggle);
void pruneParcelSelection(ParcelSelectionState& selection, size_t parcel_count);
