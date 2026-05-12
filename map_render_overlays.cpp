#include "map_render_overlays.h"

#include "map_render_utils.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace {
bool layerFillEnabled(const MapRenderContext& ctx, int layer_idx) {
    return layer_idx >= 0 &&
           ctx.layer_fill_enabled &&
           (size_t)layer_idx < ctx.layer_fill_enabled->size() &&
           (*ctx.layer_fill_enabled)[(size_t)layer_idx];
}

bool featureOnScreen(const MapRenderContext& ctx, size_t layer_idx, uint32_t feature_idx, const LayerDef::FeatureGeom& fg) {
    const auto pww = ctx.projection->getWorldExtent(layer_idx, feature_idx, fg);
    ImVec2 a = ctx.projection->projectWorld(pww.first);
    ImVec2 b = ctx.projection->projectWorld(pww.second);
    ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
    ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
    return !(p1.x < ctx.origin.x || p0.x > ctx.origin.x + ctx.size.x ||
             p1.y < ctx.origin.y || p0.y > ctx.origin.y + ctx.size.y);
}

int valueAt(const std::vector<int>* values, size_t idx) {
    return values && idx < values->size() ? (*values)[idx] : 0;
}

double parcelAreaSqM(const LayerDef::FeatureGeom& fg) {
    if (fg.rings.empty()) return 0.0;
    constexpr double kDegToMetersLat = 111320.0;
    double total = 0.0;
    for (const auto& ring : fg.rings) {
        if (ring.size() < 3) continue;
        double lat_sum = 0.0;
        for (const auto& p : ring) lat_sum += (double)p.y;
        const double lat0 = lat_sum / (double)ring.size();
        const double sx = kDegToMetersLat * std::cos(lat0 * std::numbers::pi / 180.0);
        double a = 0.0;
        for (size_t i = 0, n = ring.size(); i < n; ++i) {
            const auto& p = ring[i];
            const auto& q = ring[(i + 1) % n];
            a += ((double)p.x * sx) * ((double)q.y * kDegToMetersLat) -
                 ((double)q.x * sx) * ((double)p.y * kDegToMetersLat);
        }
        total += std::abs(a) * 0.5;
    }
    return total;
}

double parcelParameterValue(const MapRenderContext& ctx, size_t parcel_idx, const LayerDef::FeatureGeom& fg) {
    switch (ctx.parcel_parameter_mode) {
        case 1:
            return parcelAreaSqM(fg);
        case 2: {
            if (!ctx.unified_parcels) return 0.0;
            const UnifiedParcelRecord* rec = unifiedParcelAt(*ctx.unified_parcels, parcel_idx);
            return rec ? rec->current_value : 0.0;
        }
        default:
            return 0.0;
    }
}

void drawParcelOverlayRings(
    const MapRenderContext& ctx,
    const LayerDef::FeatureGeom& fg,
    const std::vector<std::vector<ImVec2>>& world_rings,
    ImU32 outline) {
    for (const auto& r : world_rings) {
        ctx.projection->appendWorldRingLine(r);
        const auto& scratch_line = ctx.projection->scratchLine();
        if (!scratch_line.empty()) {
            ctx.draw->AddPolyline(scratch_line.data(), (int)scratch_line.size(), outline, ImDrawFlags_Closed, 2.0f);
        }
    }
    (void)fg;
}
}

MapOverlayResult renderParcelSourceOverlays(const MapRenderContext& ctx) {
    MapOverlayResult result;
    if (!ctx.draw || !ctx.layers || !ctx.projection || ctx.parcel_layer_idx >= ctx.layers->size()) return result;
    if (!ctx.feature_passes_filters || !ctx.should_fill_layer_polygon) {
        return result;
    }

    auto& parcel_layer = (*ctx.layers)[ctx.parcel_layer_idx];

    if (ctx.parcel_parameter_mode > 0 && ctx.should_fill_layer_polygon(ctx.parcel_layer_idx)) {
        double min_v = std::numeric_limits<double>::infinity();
        double max_v = -std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
            const auto& fg = parcel_layer.features[i];
            if (!ctx.feature_passes_filters(ctx.parcel_layer_idx, i, fg)) continue;
            if (fg.rings.empty()) continue;
            const double v = parcelParameterValue(ctx, i, fg);
            if (v <= 0.0 || !std::isfinite(v)) continue;
            min_v = std::min(min_v, v);
            max_v = std::max(max_v, v);
        }
        const bool range_valid = std::isfinite(min_v) && std::isfinite(max_v) && max_v > min_v;
        if (range_valid) {
            for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
                auto& fg = parcel_layer.features[i];
                if (!ctx.feature_passes_filters(ctx.parcel_layer_idx, i, fg)) continue;
                if (!featureOnScreen(ctx, ctx.parcel_layer_idx, (uint32_t)i, fg)) continue;
                if (fg.rings.empty()) continue;
                const double v = parcelParameterValue(ctx, i, fg);
                if (v <= 0.0 || !std::isfinite(v)) continue;
                const float t = applyPowerGamma(
                    std::clamp((float)((v - min_v) / (max_v - min_v)), 0.0f, 1.0f),
                    ctx.parcel_choropleth_gamma);
                const auto& world_rings = ctx.projection->getWorldRings(ctx.parcel_layer_idx, (uint32_t)i, fg);
                ctx.projection->drawTessellatedFill(ctx.draw, fg, world_rings, colorWithAlpha(heatColor(t), 150));
            }
        }
    }

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

            const auto& world_rings = ctx.projection->getWorldRings(ctx.parcel_layer_idx, (uint32_t)i, fg);
            const int alpha = scaledOverlayAlpha(120, 18, 120, 230, weight);
            const ImVec4 vac_base = blendVacancyColor(ctx.vacancy_notice_color, ctx.vacancy_rehab_color, vac_notice, vac_rehab);
            const ImU32 vac_fill = colorWithAlpha(vac_base, alpha);
            const ImU32 vac_outline = colorWithAlpha(darkenColor(vac_base, 0.62f), 235);
            const bool notice_fill = layerFillEnabled(ctx, ctx.vacant_notice_layer_idx);
            const bool rehab_fill = layerFillEnabled(ctx, ctx.vacant_rehab_layer_idx);
            if ((notice_fill || rehab_fill) && ctx.should_fill_layer_polygon(ctx.parcel_layer_idx)) {
                ctx.projection->drawTessellatedFill(ctx.draw, fg, world_rings, vac_fill);
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
            const auto& world_rings = ctx.projection->getWorldRings(ctx.parcel_layer_idx, (uint32_t)i, fg);
            const bool lien_fill = ctx.tax_lien_enabled && layerFillEnabled(ctx, ctx.tax_lien_layer_idx);
            const bool sale_fill = ctx.tax_sale_enabled && layerFillEnabled(ctx, ctx.tax_sale_layer_idx);
            if ((lien_fill || sale_fill) && ctx.should_fill_layer_polygon(ctx.parcel_layer_idx)) {
                const int alpha = scaledOverlayAlpha(90, 10, 90, 210, weight);
                ctx.projection->drawTessellatedFill(ctx.draw, fg, world_rings, colorWithAlpha(tax_base, alpha));
            }
            const ImU32 tax_outline = colorWithAlpha(darkenColor(tax_base, 0.58f), 240);
            drawParcelOverlayRings(ctx, fg, world_rings, tax_outline);
        }
    }

    return result;
}
