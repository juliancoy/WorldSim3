#pragma once

#include "filters.h"
#include "zoning.h"
#include "types.h"

#include <string>
#include <unordered_map>
#include <vector>

struct ActiveQueriesTabContext {
    MapFilterState* map_filter_state = nullptr;
    std::vector<QueryMapLayer>* query_layers = nullptr;
    FilterResultSet* active_result_set = nullptr;
    std::string* active_result_status = nullptr;
    std::vector<LayerDef>* layers = nullptr;
    std::unordered_map<std::string, ZoneMetadata>* zoning_metadata = nullptr;
    std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr;
    std::vector<bool>* layer_fill_enabled = nullptr;
    int zoning_layer_idx = -1;
    int crime_nibrs_layer_idx = -1;
};

void drawActiveQueriesTab(const ActiveQueriesTabContext& ctx);
