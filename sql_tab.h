#pragma once

#include "app_settings.h"
#include "types.h"
#include "filters.h"
#include "parcel_unified.h"
#include "selection.h"

#include <filesystem>
#include <vector>

class DuckDbAnalytics;
struct DuckDbSelectedParcel;

void renderSqlTab(
    DuckDbAnalytics& duckdb_analytics,
    const std::vector<LayerDef>& layers,
    const std::vector<UnifiedParcelRecord>& unified_parcels,
    const MapFilterState& map_filter_state,
    const AppSettings& app_settings,
    const std::filesystem::path& root,
    double center_lon,
    double center_lat,
    double zoom,
    const std::vector<DuckDbSelectedParcel>& selected_parcels,
    std::vector<QueryMapLayer>& query_layers,
    std::vector<QueryHistoryEntry>& query_history);

void drawSqlTab(
    DuckDbAnalytics& duckdb_analytics,
    const std::vector<LayerDef>& layers,
    const std::vector<UnifiedParcelRecord>& unified_parcels,
    const MapFilterState& map_filter_state,
    const AppSettings& app_settings,
    const std::filesystem::path& root,
    double center_lon,
    double center_lat,
    double zoom,
    const ParcelSelectionState& parcel_selection,
    std::vector<QueryMapLayer>& query_layers,
    std::vector<QueryHistoryEntry>& query_history);
