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
    std::string parcel_blocklot;
    bool has_parcel_extent = false;
    LayerDef::FeatureExtent parcel_extent;
    const LayerDef::FeatureRecord* real_property = nullptr;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
};

std::vector<ParcelTimelineEvent> buildParcelTimeline(const ParcelTimelineRequest& request);
