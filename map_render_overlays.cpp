#include "map_render_overlays.h"

#include "map_render_utils.h"

#include <algorithm>

namespace {
bool layerFillEnabled(const MapRenderContext& ctx, int layer_idx) {
    return layer_idx >= 0 &&
           ctx.layer_fill_enabled &&
           (size_t)layer_idx < ctx.layer_fill_enabled->size() &&
           (*ctx.layer_fill_enabled)[(size_t)layer_idx];
}

bool featureOnScreen(const MapRenderContext& ctx, size_t layer_idx, uint32_t feature_idx, const LayerDef::FeatureGeom& fg) {
    const auto pww = ctx.get_world_extent(layer_idx, feature_idx, fg);
    ImVec2 a = ctx.project_world(pww.first);
    ImVec2 b = ctx.project_world(pww.second);
    ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
    ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
    return !(p1.x < ctx.origin.x || p0.x > ctx.origin.x + ctx.size.x ||
             p1.y < ctx.origin.y || p0.y > ctx.origin.y + ctx.size.y);
}

int valueAt(const std::vector<int>* values, size_t idx) {
    return values && idx < values->size() ? (*values)[idx] : 0;
}

void drawParcelOverlayRings(
    const MapRenderContext& ctx,
    const LayerDef::FeatureGeom& fg,
    const std::vector<std::vector<ImVec2>>& world_rings,
    ImU32 outline) {
    for (const auto& r : world_rings) {
        ctx.append_world_ring_line(r);
        if (ctx.scratch_line && !ctx.scratch_line->empty()) {
            ctx.draw->AddPolyline(ctx.scratch_line->data(), (int)ctx.scratch_line->size(), outline, ImDrawFlags_Closed, 2.0f);
        }
    }
    (void)fg;
}
}

MapOverlayResult renderParcelSourceOverlays(const MapRenderContext& ctx) {
    MapOverlayResult result;
    if (!ctx.draw || !ctx.layers || ctx.parcel_layer_idx >= ctx.layers->size()) return result;
    if (!ctx.feature_passes_filters || !ctx.get_world_extent || !ctx.get_world_rings ||
        !ctx.project_world || !ctx.should_fill_layer_polygon || !ctx.draw_tessellated_fill ||
        !ctx.append_world_ring_line) {
        return result;
    }

    auto& parcel_layer = (*ctx.layers)[ctx.parcel_layer_idx];

    if (ctx.vacant_notice_enabled || ctx.vacant_rehab_enabled) {
        for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
            auto& fg = parcel_layer.features[i];
            if (!ctx.feature_passes_filters(ctx.parcel_layer_idx, i, fg)) continue;
            if (!featureOnScreen(ctx, ctx.parcel_layer_idx, (uint32_t)i, fg)) continue;
            if (fg.rings.empty()) continue;

            const int vac_notice = valueAt(ctx.parcel_vac_notice_by_feature, i);
            const int vac_rehab = valueAt(ctx.parcel_vac_rehab_by_feature, i);
            const int weight = overlayWeight(ctx.vacant_notice_enabled, vac_notice, ctx.vacant_rehab_enabled, vac_rehab);
            if (weight <= 0) continue;
            result.visible_vacant_parcels++;

            const auto& world_rings = ctx.get_world_rings(ctx.parcel_layer_idx, (uint32_t)i, fg);
            const int alpha = scaledOverlayAlpha(120, 18, 120, 230, weight);
            const ImVec4 vac_base = blendVacancyColor(ctx.vacancy_notice_color, ctx.vacancy_rehab_color, vac_notice, vac_rehab);
            const ImU32 vac_fill = colorWithAlpha(vac_base, alpha);
            const ImU32 vac_outline = colorWithAlpha(darkenColor(vac_base, 0.62f), 235);
            const bool notice_fill = layerFillEnabled(ctx, ctx.vacant_notice_layer_idx);
            const bool rehab_fill = layerFillEnabled(ctx, ctx.vacant_rehab_layer_idx);
            if ((notice_fill || rehab_fill) && ctx.should_fill_layer_polygon(ctx.parcel_layer_idx)) {
                ctx.draw_tessellated_fill(fg, world_rings, vac_fill);
            }
            drawParcelOverlayRings(ctx, fg, world_rings, vac_outline);
        }
    }

    if (ctx.tax_lien_enabled || ctx.tax_sale_enabled) {
        for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
            auto& fg = parcel_layer.features[i];
            if (!ctx.feature_passes_filters(ctx.parcel_layer_idx, i, fg)) continue;
            if (!featureOnScreen(ctx, ctx.parcel_layer_idx, (uint32_t)i, fg)) continue;
            if (fg.rings.empty()) continue;

            const int lien_count = valueAt(ctx.parcel_tax_lien_by_feature, i);
            const int sale_count = valueAt(ctx.parcel_tax_sale_by_feature, i);
            const int weight = overlayWeight(ctx.tax_lien_enabled, lien_count, ctx.tax_sale_enabled, sale_count);
            if (weight <= 0) continue;

            const ImVec4 lien_c = (ctx.tax_lien_layer_idx >= 0) ? (*ctx.layers)[(size_t)ctx.tax_lien_layer_idx].color : ImVec4(0.95f, 0.55f, 0.1f, 1.0f);
            const ImVec4 sale_c = (ctx.tax_sale_layer_idx >= 0) ? (*ctx.layers)[(size_t)ctx.tax_sale_layer_idx].color : ImVec4(0.85f, 0.2f, 0.1f, 1.0f);
            const ImVec4 tax_base = blendTaxColor(lien_c, sale_c, ctx.tax_lien_enabled, ctx.tax_sale_enabled, lien_count, sale_count);
            const auto& world_rings = ctx.get_world_rings(ctx.parcel_layer_idx, (uint32_t)i, fg);
            const bool lien_fill = ctx.tax_lien_enabled && layerFillEnabled(ctx, ctx.tax_lien_layer_idx);
            const bool sale_fill = ctx.tax_sale_enabled && layerFillEnabled(ctx, ctx.tax_sale_layer_idx);
            if ((lien_fill || sale_fill) && ctx.should_fill_layer_polygon(ctx.parcel_layer_idx)) {
                const int alpha = scaledOverlayAlpha(90, 10, 90, 210, weight);
                ctx.draw_tessellated_fill(fg, world_rings, colorWithAlpha(tax_base, alpha));
            }
            const ImU32 tax_outline = colorWithAlpha(darkenColor(tax_base, 0.58f), 240);
            drawParcelOverlayRings(ctx, fg, world_rings, tax_outline);
        }
    }

    return result;
}
