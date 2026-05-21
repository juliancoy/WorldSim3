#include "selection.h"

#include "app_utils.h"

#include <algorithm>
#include <unordered_map>

void clearParcelSelection(ParcelSelectionState& selection) {
    selection.show_details = false;
    selection.active_idx = (size_t)-1;
    selection.active_stable_id.clear();
    selection.indices.clear();
    selection.index_set.clear();
    selection.stable_ids.clear();
    selection.stable_id_set.clear();
}

bool selectParcel(
    ParcelSelectionState& selection,
    size_t idx,
    const std::string& stable_id,
    size_t parcel_count,
    bool append_toggle) {
    if (idx >= parcel_count) return false;
    if (!append_toggle) clearParcelSelection(selection);

    const auto it = selection.index_set.find(idx);
    const auto stable_it = stable_id.empty() ? selection.stable_id_set.end() : selection.stable_id_set.find(stable_id);
    if (append_toggle && (it != selection.index_set.end() || stable_it != selection.stable_id_set.end())) {
        if (it != selection.index_set.end()) selection.index_set.erase(it);
        selection.indices.erase(std::remove(selection.indices.begin(), selection.indices.end(), idx), selection.indices.end());
        if (!stable_id.empty()) {
            selection.stable_id_set.erase(stable_id);
            selection.stable_ids.erase(
                std::remove(selection.stable_ids.begin(), selection.stable_ids.end(), stable_id),
                selection.stable_ids.end());
        }
        if (selection.indices.empty()) {
            selection.show_details = false;
            selection.active_idx = (size_t)-1;
            selection.active_stable_id.clear();
        } else {
            selection.show_details = true;
            selection.active_idx = selection.indices.back();
            selection.active_stable_id = selection.stable_ids.empty() ? std::string() : selection.stable_ids.back();
        }
        return true;
    }

    selection.index_set.insert(idx);
    selection.indices.push_back(idx);
    if (!stable_id.empty() && selection.stable_id_set.insert(stable_id).second) {
        selection.stable_ids.push_back(stable_id);
    }
    selection.show_details = true;
    selection.active_idx = idx;
    selection.active_stable_id = stable_id;
    return true;
}

bool selectParcel(ParcelSelectionState& selection, size_t idx, size_t parcel_count, bool append_toggle) {
    return selectParcel(selection, idx, {}, parcel_count, append_toggle);
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
        selection.active_stable_id.clear();
    } else {
        selection.show_details = true;
        selection.active_idx = selection.indices.back();
        if (selection.active_stable_id.empty() && !selection.stable_ids.empty()) {
            selection.active_stable_id = selection.stable_ids.back();
        }
    }
}

void reconcileParcelSelection(ParcelSelectionState& selection, const LayerDef& parcel_layer) {
    if (selection.stable_ids.empty()) {
        selection.index_set.clear();
        for (size_t idx : selection.indices) {
            if (idx >= parcel_layer.features.size()) continue;
            selection.index_set.insert(idx);
            const std::string stable_id =
                featureStableIdForLayerFeature(parcel_layer, parcel_layer.features[idx], idx);
            if (!stable_id.empty() && selection.stable_id_set.insert(stable_id).second) {
                selection.stable_ids.push_back(stable_id);
            }
        }
        if (selection.active_idx < parcel_layer.features.size()) {
            selection.active_stable_id = featureStableIdForLayerFeature(
                parcel_layer,
                parcel_layer.features[selection.active_idx],
                selection.active_idx);
        }
        pruneParcelSelection(selection, parcel_layer.features.size());
        return;
    }

    std::unordered_map<std::string, size_t> idx_by_stable_id;
    idx_by_stable_id.reserve(parcel_layer.features.size());
    for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
        idx_by_stable_id.emplace(featureStableIdForLayerFeature(parcel_layer, parcel_layer.features[i], i), i);
    }

    selection.indices.clear();
    selection.index_set.clear();
    std::vector<std::string> resolved_stable_ids;
    resolved_stable_ids.reserve(selection.stable_ids.size());
    for (const std::string& stable_id : selection.stable_ids) {
        auto it = idx_by_stable_id.find(stable_id);
        if (it == idx_by_stable_id.end()) continue;
        selection.indices.push_back(it->second);
        selection.index_set.insert(it->second);
        resolved_stable_ids.push_back(stable_id);
    }
    selection.stable_ids = std::move(resolved_stable_ids);
    selection.stable_id_set.clear();
    for (const std::string& stable_id : selection.stable_ids) selection.stable_id_set.insert(stable_id);

    if (selection.indices.empty()) {
        selection.show_details = false;
        selection.active_idx = (size_t)-1;
        selection.active_stable_id.clear();
        return;
    }

    selection.show_details = true;
    if (!selection.active_stable_id.empty()) {
        auto it = idx_by_stable_id.find(selection.active_stable_id);
        selection.active_idx = (it != idx_by_stable_id.end()) ? it->second : selection.indices.back();
    } else {
        selection.active_idx = selection.indices.back();
        selection.active_stable_id = selection.stable_ids.back();
    }
}
