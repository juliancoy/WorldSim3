#include "selection.h"

#include <algorithm>

void clearParcelSelection(ParcelSelectionState& selection) {
    selection.show_details = false;
    selection.active_idx = (size_t)-1;
    selection.indices.clear();
    selection.index_set.clear();
}

bool selectParcel(ParcelSelectionState& selection, size_t idx, size_t parcel_count, bool append_toggle) {
    if (idx >= parcel_count) return false;
    if (!append_toggle) clearParcelSelection(selection);

    const auto it = selection.index_set.find(idx);
    if (append_toggle && it != selection.index_set.end()) {
        selection.index_set.erase(it);
        selection.indices.erase(std::remove(selection.indices.begin(), selection.indices.end(), idx), selection.indices.end());
        if (selection.indices.empty()) {
            selection.show_details = false;
            selection.active_idx = (size_t)-1;
        } else {
            selection.show_details = true;
            selection.active_idx = selection.indices.back();
        }
        return true;
    }

    selection.index_set.insert(idx);
    selection.indices.push_back(idx);
    selection.show_details = true;
    selection.active_idx = idx;
    return true;
}

void pruneParcelSelection(ParcelSelectionState& selection, size_t parcel_count) {
    selection.indices.erase(std::remove_if(selection.indices.begin(), selection.indices.end(), [&](size_t idx) {
        return idx >= parcel_count;
    }), selection.indices.end());

    selection.index_set.clear();
    for (size_t idx : selection.indices) selection.index_set.insert(idx);

    if (selection.indices.empty()) {
        selection.show_details = false;
        selection.active_idx = (size_t)-1;
    } else {
        selection.show_details = true;
        selection.active_idx = selection.indices.back();
    }
}
