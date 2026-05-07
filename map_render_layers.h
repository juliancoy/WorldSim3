#pragma once

#include <cstddef>

struct RawSourceLayerPolicy {
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
};

bool shouldSkipRawSourceLayer(size_t layer_idx, const RawSourceLayerPolicy& policy);
