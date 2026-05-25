#include "selection.h"

#include "app_utils.h"
#include "feature_props.h"

#include <algorithm>
#include <unordered_map>

namespace {
void syncSelectionViews(ParcelSelectionState& selection) {
    selection.entity_ids.clear();
    selection.entity_id_set.clear();
    for (const ParcelSelectionRef& ref : selection.refs) {
        if (!ref.entity_id.empty() && selection.entity_id_set.insert(ref.entity_id).second) {
            selection.entity_ids.push_back(ref.entity_id);
        }
    }
    if (selection.refs.empty()) {
        selection.show_details = false;
        selection.active_layer_idx = -1;
        selection.active_entity_id.clear();
        return;
    }
    selection.show_details = true;
    selection.active_layer_idx = selection.refs.back().layer_idx;
    selection.active_entity_id = selection.refs.back().entity_id;
}

bool sameSelectionRef(const ParcelSelectionRef& ref, int layer_idx, const std::string& entity_id) {
    return ref.layer_idx == layer_idx && !entity_id.empty() && ref.entity_id == entity_id;
}
}

void clearParcelSelection(ParcelSelectionState& selection) {
    selection.show_details = false;
    selection.active_layer_idx = -1;
    selection.active_entity_id.clear();
    selection.refs.clear();
    selection.entity_ids.clear();
    selection.entity_id_set.clear();
}

bool selectParcel(
    ParcelSelectionState& selection,
    int layer_idx,
    const std::string& entity_id,
    bool append_toggle) {
    if (entity_id.empty()) return false;
    if (!append_toggle) clearParcelSelection(selection);

    auto ref_it = std::find_if(
        selection.refs.begin(),
        selection.refs.end(),
        [&](const ParcelSelectionRef& ref) {
            return sameSelectionRef(ref, layer_idx, entity_id);
        });
    if (append_toggle && ref_it != selection.refs.end()) {
        selection.refs.erase(ref_it);
        syncSelectionViews(selection);
        return true;
    }

    selection.refs.push_back(ParcelSelectionRef{
        layer_idx,
        entity_id
    });
    syncSelectionViews(selection);
    return true;
}

void pruneParcelSelection(ParcelSelectionState& selection, const std::vector<LayerDef>& layers) {
    selection.refs.erase(
        std::remove_if(
            selection.refs.begin(),
            selection.refs.end(),
            [&](const ParcelSelectionRef& ref) {
                if (ref.layer_idx < 0) return true;
                if ((size_t)ref.layer_idx >= layers.size()) return true;
                return featureIndexForEntityId(layers[(size_t)ref.layer_idx], ref.entity_id) == (size_t)-1;
            }),
        selection.refs.end());
    syncSelectionViews(selection);
}

void reconcileParcelSelection(ParcelSelectionState& selection, const std::vector<LayerDef>& layers) {
    for (ParcelSelectionRef& ref : selection.refs) {
        if (ref.layer_idx < 0 || (size_t)ref.layer_idx >= layers.size()) continue;
        ref.entity_id = normalizeJoinKey(ref.entity_id);
    }
    pruneParcelSelection(selection, layers);
}
