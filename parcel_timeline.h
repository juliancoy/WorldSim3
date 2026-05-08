#pragma once

#include "types.h"

#include <string>
#include <vector>

struct ParcelTimelineEvent {
    std::string date;
    std::string event_type;
    std::string status;
    std::string amount;
    std::string source_layer;
    std::string sort_key;
};

struct ParcelTimelineRequest {
    const std::vector<LayerDef>* layers = nullptr;
    const LayerDef::FeatureGeom* parcel = nullptr;
    const LayerDef::FeatureGeom* real_property = nullptr;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
};

std::vector<ParcelTimelineEvent> buildParcelTimeline(const ParcelTimelineRequest& request);
