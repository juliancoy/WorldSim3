#include "render_tail_pass.h"

#include "map_render_hud.h"
#include "map_render_overlays.h"
#include "map_render_selection.h"
#include "worldsim_app.h"

RenderTailPassResult runRenderTailPass(const RenderTailPassContext& ctx) {
    MapRenderContext overlay_ctx;
    overlay_ctx.draw = ctx.draw;
    overlay_ctx.origin = ctx.origin;
    overlay_ctx.size = ctx.size;
    overlay_ctx.layers = ctx.layers;
    overlay_ctx.parcel_layer_idx = ctx.parcel_layer_idx >= 0 ? (size_t)ctx.parcel_layer_idx : (size_t)-1;
    overlay_ctx.vacant_notice_enabled = ctx.vacant_notice_overlay_enabled;
    overlay_ctx.vacant_rehab_enabled = ctx.vacant_rehab_overlay_enabled;
    overlay_ctx.tax_lien_enabled = ctx.tax_lien_overlay_enabled;
    overlay_ctx.tax_sale_enabled = ctx.tax_sale_overlay_enabled;
    overlay_ctx.vacant_notice_layer_idx = ctx.vacant_notice_layer_idx;
    overlay_ctx.vacant_rehab_layer_idx = ctx.vacant_rehab_layer_idx;
    overlay_ctx.tax_lien_layer_idx = ctx.tax_lien_layer_idx;
    overlay_ctx.tax_sale_layer_idx = ctx.tax_sale_layer_idx;
    overlay_ctx.vacancy_notice_color = ctx.vacancy_notice_color ? *ctx.vacancy_notice_color : ImVec4(1, 0, 0, 1);
    overlay_ctx.vacancy_rehab_color = ctx.vacancy_rehab_color ? *ctx.vacancy_rehab_color : ImVec4(0, 1, 1, 1);
    overlay_ctx.layer_fill_enabled = ctx.layer_fill_enabled;
    overlay_ctx.parcel_vac_notice_by_feature = ctx.parcel_vac_notice_by_feature;
    overlay_ctx.parcel_vac_rehab_by_feature = ctx.parcel_vac_rehab_by_feature;
    overlay_ctx.parcel_tax_lien_by_feature = ctx.parcel_tax_lien_by_feature;
    overlay_ctx.parcel_tax_sale_by_feature = ctx.parcel_tax_sale_by_feature;
    overlay_ctx.unified_parcels = ctx.unified_parcels;
    overlay_ctx.parcel_parameter_mode = ctx.parcel_parameter_mode;
    overlay_ctx.parcel_choropleth_gamma = ctx.parcel_choropleth_gamma;
    overlay_ctx.feature_passes_filters = ctx.feature_passes_filters;
    overlay_ctx.should_fill_layer_polygon = ctx.should_fill_layer_polygon;
    overlay_ctx.projection = ctx.projection;

    MapOverlayResult overlay_result;
    const bool gpu_overlay_active = parcelGpuOverlayDrawActive();
    const bool gpu_outline_active = parcelGpuOutlineDrawActive();
    if (!gpu_overlay_active && !gpu_outline_active) {
        overlay_result = renderParcelSourceOverlays(overlay_ctx);
    }
    if (gpu_overlay_active) {
        enqueueParcelGpuOverlayDraw(ctx.draw);
    }
    if (gpu_outline_active) {
        enqueueParcelGpuOutlineDraw(ctx.draw);
    }
    renderSelectedParcelOutlines(MapSelectionRenderContext{
        ctx.draw,
        ctx.origin,
        ctx.size,
        ctx.layers,
        ctx.parcel_layer_idx,
        ctx.selected_parcel_indices,
        ctx.math_zoom,
        ctx.projection,
        ctx.project_world
    });
    drawMapZoomBadge(ctx.draw, ctx.origin, ctx.size, ctx.zoom);

    RenderTailPassResult result;
    result.visible_vacant_parcels = overlay_result.visible_vacant_parcels;
    result.fill_stats = ctx.projection->fillStats();
    return result;
}
