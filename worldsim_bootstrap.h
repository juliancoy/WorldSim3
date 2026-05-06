#pragma once

#include "layer_runtime.h"

#include <vector>

struct WorldsimLayerIndices {
    int parcel_layer_idx = -1;
    int real_property_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
    int zoning_layer_idx = -1;
    int crime_nibrs_layer_idx = -1;
    int crime_legacy_layer_idx = -1;
};

WorldsimLayerIndices findWorldsimLayerIndices(const std::vector<LayerDef>& layers);
