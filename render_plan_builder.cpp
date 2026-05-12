#include "render_plan_builder.h"

RenderPlan buildRenderPlan(const RenderPlanBuilderContext& ctx) {
    RenderPlan plan;
    if (!ctx.layers) return plan;

    plan.overlays.vacant_notice_overlay_enabled =
        ctx.vacant_notice_enabled &&
        !(ctx.vacant_notice_layer_idx >= 0 &&
          (ctx.layer_uses_heatmap_aggregate((size_t)ctx.vacant_notice_layer_idx) ||
           ctx.layer_uses_lod_geometry((size_t)ctx.vacant_notice_layer_idx)));
    plan.overlays.vacant_rehab_overlay_enabled =
        ctx.vacant_rehab_enabled &&
        !(ctx.vacant_rehab_layer_idx >= 0 &&
          (ctx.layer_uses_heatmap_aggregate((size_t)ctx.vacant_rehab_layer_idx) ||
           ctx.layer_uses_lod_geometry((size_t)ctx.vacant_rehab_layer_idx)));
    plan.overlays.tax_lien_overlay_enabled =
        ctx.tax_lien_enabled &&
        !(ctx.tax_lien_layer_idx >= 0 &&
          (ctx.layer_uses_heatmap_aggregate((size_t)ctx.tax_lien_layer_idx) ||
           ctx.layer_uses_lod_geometry((size_t)ctx.tax_lien_layer_idx)));
    plan.overlays.tax_sale_overlay_enabled =
        ctx.tax_sale_enabled &&
        !(ctx.tax_sale_layer_idx >= 0 &&
          (ctx.layer_uses_heatmap_aggregate((size_t)ctx.tax_sale_layer_idx) ||
           ctx.layer_uses_lod_geometry((size_t)ctx.tax_sale_layer_idx)));

    plan.draw_layer_order.reserve(ctx.layers->size());
    std::vector<size_t> parcel_related_draw_layers;
    parcel_related_draw_layers.reserve(5);
    size_t zoning_draw_layer = (size_t)-1;
    for (size_t layer_idx = 0; layer_idx < ctx.layers->size(); ++layer_idx) {
        if ((int)layer_idx == ctx.zoning_layer_idx) {
            zoning_draw_layer = layer_idx;
            continue;
        }
        if (ctx.is_parcel_related_layer && ctx.is_parcel_related_layer(layer_idx)) {
            parcel_related_draw_layers.push_back(layer_idx);
            continue;
        }
        plan.draw_layer_order.push_back(layer_idx);
    }
    if (zoning_draw_layer != (size_t)-1) plan.draw_layer_order.push_back(zoning_draw_layer);
    plan.draw_layer_order.insert(
        plan.draw_layer_order.end(),
        parcel_related_draw_layers.begin(),
        parcel_related_draw_layers.end());
    return plan;
}
