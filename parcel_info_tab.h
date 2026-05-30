#pragma once

#include "duckdb_analytics.h"
#include "owner_info.h"
#include "parcel_unified.h"
#include "selection.h"
#include "types.h"

#include <cstddef>
#include <functional>
#include <vector>

struct ParcelInfoTabContext {
    const std::vector<LayerDef>* layers = nullptr;
    const std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;
    DuckDbAnalytics* duckdb_analytics = nullptr;
    OwnerInfoUiState* owner_info_state = nullptr;
    int parcel_layer_idx = -1;
    const ParcelSelectionState* parcel_selection = nullptr;
    bool show_selected_parcel_details = false;
    const std::unordered_map<std::string, size_t>* real_property_by_blocklot = nullptr;
    int real_property_layer_idx = -1;
    bool* tab_requested = nullptr;
    std::function<void()> clear_parcel_selection;
    std::function<bool(const std::string&, bool)> select_parcel_id;
};

void drawParcelInfoTab(const ParcelInfoTabContext& ctx);
