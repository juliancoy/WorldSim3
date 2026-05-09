#pragma once

#include "owners_tab.h"

#include <cstddef>

struct VacancyParcelTabContext {
    size_t notices_total = 0;
    size_t rehabs_total = 0;
    size_t notices_matched = 0;
    size_t rehabs_matched = 0;
    size_t parcels_matched = 0;
    size_t parcels_with_geometry = 0;
    bool has_selected_owners = false;
    const FilteredAggregateSnapshot* filtered_aggregate_snapshot = nullptr;
};

void drawVacancyParcelTab(const VacancyParcelTabContext& ctx);
