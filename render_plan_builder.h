#pragma once

#include "map_render_layers.h"
#include "types.h"

#include <cstddef>
#include <functional>
#include <vector>

struct RenderOverlayPolicy {
    bool vacant_notice_overlay_enabled = false;
    bool vacant_rehab_overlay_enabled = false;
    bool tax_lien_overlay_enabled = false;
    bool tax_sale_overlay_enabled = false;
};

struct RenderPlan {
    std::vector<size_t> draw_layer_order;
    RenderOverlayPolicy overlays;
};

struct RenderPlanBuilderContext {
    const std::vector<LayerDef>* layers = nullptr;
    RawSourceLayerPolicy raw_source_layer_policy;
    int zoning_layer_idx = -1;
    bool vacant_notice_enabled = false;
    bool vacant_rehab_enabled = false;
    bool tax_lien_enabled = false;
    bool tax_sale_enabled = false;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
    std::function<bool(size_t)> is_parcel_related_layer;
    std::function<bool(size_t)> layer_uses_heatmap_aggregate;
    std::function<bool(size_t)> layer_uses_lod_geometry;
};

RenderPlan buildRenderPlan(const RenderPlanBuilderContext& ctx);
