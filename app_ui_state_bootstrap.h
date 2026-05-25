#pragma once

#include "filters.h"
#include "selection.h"

#include <cstddef>
#include <filesystem>
#include <vector>

struct AppUiStateBootstrapInput {
    const std::filesystem::path* root = nullptr;
    const std::vector<LayerDef>* layers = nullptr;
    LayerBrowseState* layer_browse_state = nullptr;
    MapFilterState* map_filter_state = nullptr;
    char* owner_search_query = nullptr;
    size_t owner_search_query_size = 0;
    std::vector<QueryHistoryEntry>* query_history = nullptr;
    double* center_lon = nullptr;
    double* center_lat = nullptr;
    double* zoom = nullptr;
    int* zoning_layer_idx = nullptr;
    int fallback_zoning_layer_idx = -1;
    ParcelSelectionState* parcel_selection = nullptr;
};

int resolveActiveZoningLayerIndex(
    const std::vector<LayerDef>& layers,
    int zoning_layer_idx,
    int fallback_zoning_layer_idx);
void loadPersistedAppUiState(AppUiStateBootstrapInput& input);
