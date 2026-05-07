#include "map_render_layers.h"

bool shouldSkipRawSourceLayer(size_t layer_idx, const RawSourceLayerPolicy& policy) {
    return (policy.vacant_notice_layer_idx >= 0 && (int)layer_idx == policy.vacant_notice_layer_idx) ||
           (policy.vacant_rehab_layer_idx >= 0 && (int)layer_idx == policy.vacant_rehab_layer_idx) ||
           (policy.tax_lien_layer_idx >= 0 && (int)layer_idx == policy.tax_lien_layer_idx) ||
           (policy.tax_sale_layer_idx >= 0 && (int)layer_idx == policy.tax_sale_layer_idx);
}
