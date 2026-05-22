#pragma once

#include "filters.h"
#include "types.h"

#include <string>
#include <vector>

struct EventSectorFiltersPanelContext {
    const std::vector<LayerDef>* layers = nullptr;
    const MapFilterState* map_filter_state = nullptr;
};

bool drawEventSectorFiltersPanel(EventSectorFiltersPanelContext& ctx);
