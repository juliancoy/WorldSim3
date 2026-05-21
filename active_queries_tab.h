#pragma once

#include "app_settings.h"
#include "duckdb_analytics.h"
#include "filters.h"
#include "zoning.h"
#include "types.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct ActiveQueriesTabContext {
    MapFilterState* map_filter_state = nullptr;
    AppSettings* app_settings = nullptr;
    const std::filesystem::path* root = nullptr;
    std::vector<QueryMapLayer>* query_layers = nullptr;
    std::vector<QueryHistoryEntry>* query_history = nullptr;
    DuckDbAnalytics* duckdb_analytics = nullptr;
    FilterResultSet* active_result_set = nullptr;
    std::string* active_result_status = nullptr;
    std::vector<LayerDef>* layers = nullptr;
    std::unordered_map<std::string, ZoneMetadata>* zoning_metadata = nullptr;
    std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr;
    std::vector<bool>* layer_fill_enabled = nullptr;
    double* center_lon = nullptr;
    double* center_lat = nullptr;
    double* zoom = nullptr;
    int zoning_layer_idx = -1;
    int crime_nibrs_layer_idx = -1;
};

void drawActiveQueriesTab(const ActiveQueriesTabContext& ctx);
void drawQueryHistoryTab(ActiveQueriesTabContext& ctx);
