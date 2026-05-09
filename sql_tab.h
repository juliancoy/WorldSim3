#pragma once

#include "types.h"
#include "filters.h"
#include "parcel_unified.h"

#include <vector>

class DuckDbAnalytics;
struct DuckDbSelectedParcel;

void renderSqlTab(
    DuckDbAnalytics& duckdb_analytics,
    const std::vector<LayerDef>& layers,
    const std::vector<UnifiedParcelRecord>& unified_parcels,
    const MapFilterState& map_filter_state,
    const std::vector<DuckDbSelectedParcel>& selected_parcels,
    std::vector<QueryMapLayer>& query_layers);
