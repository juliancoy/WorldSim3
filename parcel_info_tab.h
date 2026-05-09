#pragma once

#include "owner_info.h"
#include "parcel_unified.h"
#include "types.h"

#include <cstddef>
#include <functional>
#include <vector>

struct ParcelInfoTabContext {
    const std::vector<LayerDef>* layers = nullptr;
    const std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;
    OwnerInfoUiState* owner_info_state = nullptr;
    int parcel_layer_idx = -1;
    size_t selected_parcel_idx = (size_t)-1;
    const std::vector<size_t>* selected_parcel_indices = nullptr;
    bool show_selected_parcel_details = false;
    bool* tab_requested = nullptr;
    std::function<void()> clear_parcel_selection;
    std::function<const LayerDef::FeatureGeom*(const LayerDef::FeatureGeom&)> real_property_for_parcel;
};

void drawParcelInfoTab(const ParcelInfoTabContext& ctx);
