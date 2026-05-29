#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ParcelSelectionRef {
    int layer_idx = -1;
    std::string geometry_entity_id;
    std::string entity_id;
    size_t feature_idx = (size_t)-1;
};

struct ParcelSelectionState {
    bool show_details = false;
    int active_layer_idx = -1;
    std::string active_entity_id;
    std::vector<ParcelSelectionRef> refs;
    std::vector<std::string> entity_ids;
    std::unordered_set<std::string> entity_id_set;
};

void clearParcelSelection(ParcelSelectionState& selection);
bool selectParcel(
    ParcelSelectionState& selection,
    int layer_idx,
    const std::string& entity_id,
    bool append_toggle);
bool selectParcel(
    ParcelSelectionState& selection,
    int layer_idx,
    const std::string& geometry_entity_id,
    const std::string& entity_id,
    bool append_toggle);
bool selectParcel(
    ParcelSelectionState& selection,
    int layer_idx,
    size_t feature_idx,
    const std::string& geometry_entity_id,
    const std::string& entity_id,
    bool append_toggle);
void pruneParcelSelection(ParcelSelectionState& selection, const std::vector<LayerDef>& layers);
void reconcileParcelSelection(ParcelSelectionState& selection, const std::vector<LayerDef>& layers);
