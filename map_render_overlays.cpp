#include "map_render_overlays.h"

#include "choropleth_histogram.h"
#include "geo.h"
#include "map_render_utils.h"
#include "parcel_metrics.h"
#include "worldsim_app.h"

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

bool featureOnScreen(const MapRenderContext& ctx, size_t layer_idx, uint32_t feature_idx, const LayerDef::FeatureRecord& fg) {
    if (ctx.parcel_render_blob && layer_idx == ctx.parcel_layer_idx && feature_idx < ctx.parcel_render_blob->features.size()) {
        const ParcelRenderFeatureRecord& rec = ctx.parcel_render_blob->features[feature_idx];
        ImVec2 a = ctx.projection->projectWorld(lonLatToWorldPx(rec.min_lon, rec.max_lat, ctx.math_zoom));
        ImVec2 b = ctx.projection->projectWorld(lonLatToWorldPx(rec.max_lon, rec.min_lat, ctx.math_zoom));
        ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
        ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
        return !(p1.x < ctx.origin.x || p0.x > ctx.origin.x + ctx.size.x ||
                 p1.y < ctx.origin.y || p0.y > ctx.origin.y + ctx.size.y);
    }
    if (layer_idx == ctx.parcel_layer_idx) return false;
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

const ParcelRenderFeatureRecord* parcelRenderFeature(const MapRenderContext& ctx, size_t parcel_idx) {
    if (!ctx.parcel_render_blob) return nullptr;
    if (parcel_idx < ctx.parcel_render_blob->features.size() &&
        ctx.parcel_render_blob->features[parcel_idx].feature_idx == parcel_idx) {
        return &ctx.parcel_render_blob->features[parcel_idx];
    }
    for (const ParcelRenderFeatureRecord& rec : ctx.parcel_render_blob->features) {
        if (rec.feature_idx == parcel_idx) return &rec;
    }
    return nullptr;
}

bool parcelOverlayHasGeometry(const MapRenderContext& ctx, size_t parcel_idx, const LayerDef::FeatureRecord& fg) {
    (void)fg;
    return parcelRenderFeature(ctx, parcel_idx) != nullptr;
}

void drawParcelOverlayRings(
    const MapRenderContext& ctx,
    size_t parcel_idx,
    const LayerDef::FeatureRecord& fg,
    const std::vector<std::vector<ImVec2>>& world_rings,
    ImU32 outline) {
    if (parcelGpuOutlineDrawActive()) {
        (void)ctx;
        (void)fg;
        (void)world_rings;
        (void)outline;
        return;
    }
    if (const ParcelRenderFeatureRecord* rec = parcelRenderFeature(ctx, parcel_idx)) {
        if (!ctx.parcel_render_blob) return;
        const uint32_t end = rec->line_index_offset + rec->line_index_count;
        if (end <= ctx.parcel_render_blob->line_indices.size()) {
            for (uint32_t i = rec->line_index_offset; i + 1 < end; i += 2) {
                const uint32_t ia = ctx.parcel_render_blob->line_indices[i];
                const uint32_t ib = ctx.parcel_render_blob->line_indices[i + 1];
                if (ia >= ctx.parcel_render_blob->vertices.size() || ib >= ctx.parcel_render_blob->vertices.size()) continue;
                ctx.draw->AddLine(
                    ctx.projection->projectWorld(lonLatToWorldPx(ctx.parcel_render_blob->vertices[ia].x, ctx.parcel_render_blob->vertices[ia].y, ctx.math_zoom)),
                    ctx.projection->projectWorld(lonLatToWorldPx(ctx.parcel_render_blob->vertices[ib].x, ctx.parcel_render_blob->vertices[ib].y, ctx.math_zoom)),
                    outline,
                    2.0f);
            }
            (void)fg;
            return;
        }
    }
    (void)fg;
    (void)world_rings;
}
}

MapOverlayResult renderParcelSourceOverlays(const MapRenderContext& ctx) {
    MapOverlayResult result;
    if (!ctx.draw || !ctx.layers || !ctx.projection || ctx.parcel_layer_idx >= ctx.layers->size()) return result;
    if (!(*ctx.layers)[ctx.parcel_layer_idx].enabled) return result;
    if (!ctx.feature_passes_filters || !ctx.should_fill_layer_polygon) {
        return result;
    }

    auto& parcel_layer = (*ctx.layers)[ctx.parcel_layer_idx];

    if (ctx.parcel_parameter_mode > 0 && ctx.should_fill_layer_polygon(ctx.parcel_layer_idx)) {
        std::vector<double> values;
        values.reserve(parcel_layer.features.size());
        for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
            const auto& fg = parcel_layer.features[i];
            if (!ctx.feature_passes_filters(ctx.parcel_layer_idx, i, fg)) continue;
            if (!parcelOverlayHasGeometry(ctx, i, fg)) continue;
            const double v = parcelParameterValue(
                ctx.parcel_parameter_mode,
                ctx.unified_parcels,
                ctx.parcel_render_blob,
                i,
                fg);
            if (v > 0.0 && std::isfinite(v)) values.push_back(v);
        }
        const int normalize_mode =
            ctx.layer_normalize_mode && ctx.parcel_layer_idx < ctx.layer_normalize_mode->size()
                ? std::clamp((*ctx.layer_normalize_mode)[ctx.parcel_layer_idx], 0, 3)
                : 1;
        const float clip_pct =
            ctx.layer_heatmap_percentile_clip && ctx.parcel_layer_idx < ctx.layer_heatmap_percentile_clip->size()
                ? (*ctx.layer_heatmap_percentile_clip)[ctx.parcel_layer_idx]
                : 100.0f;
        const ApproxHistogram hist = buildApproxHistogram(values, clip_pct);
        if (hist.rangeValid()) {
            for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
                auto& fg = parcel_layer.features[i];
                if (!ctx.feature_passes_filters(ctx.parcel_layer_idx, i, fg)) continue;
                if (!featureOnScreen(ctx, ctx.parcel_layer_idx, (uint32_t)i, fg)) continue;
                if (!parcelOverlayHasGeometry(ctx, i, fg)) continue;
                const double v = parcelParameterValue(
                    ctx.parcel_parameter_mode,
                    ctx.unified_parcels,
                    ctx.parcel_render_blob,
                    i,
                    fg);
                if (v <= 0.0 || !std::isfinite(v)) continue;
                const float normalized =
                    normalize_mode == 0
                        ? hist.normalizeLinear(v)
                        : (normalize_mode == 3 ? hist.normalizeEqualCountZones(v) : hist.normalizeApproxPercentile(v));
                const float t = applyPowerGamma(
                    normalized,
                    ctx.parcel_choropleth_gamma);
                if (!parcelGpuOverlayDrawActive()) {
                    if (const ParcelRenderFeatureRecord* rec = parcelRenderFeature(ctx, i)) {
                        const uint32_t end = rec->index_offset + rec->index_count;
                        if (ctx.parcel_render_blob && end <= ctx.parcel_render_blob->indices.size()) {
                            const ImU32 fill = colorWithAlpha(heatColor(t), 150);
                            for (uint32_t ti = rec->index_offset; ti + 2 < end; ti += 3) {
                                const uint32_t ia = ctx.parcel_render_blob->indices[ti];
                                const uint32_t ib = ctx.parcel_render_blob->indices[ti + 1];
                                const uint32_t ic = ctx.parcel_render_blob->indices[ti + 2];
                                if (ia >= ctx.parcel_render_blob->vertices.size() ||
                                    ib >= ctx.parcel_render_blob->vertices.size() ||
                                    ic >= ctx.parcel_render_blob->vertices.size()) continue;
                                ctx.draw->AddTriangleFilled(
                                    ctx.projection->projectWorld(lonLatToWorldPx(ctx.parcel_render_blob->vertices[ia].x, ctx.parcel_render_blob->vertices[ia].y, ctx.math_zoom)),
                                    ctx.projection->projectWorld(lonLatToWorldPx(ctx.parcel_render_blob->vertices[ib].x, ctx.parcel_render_blob->vertices[ib].y, ctx.math_zoom)),
                                    ctx.projection->projectWorld(lonLatToWorldPx(ctx.parcel_render_blob->vertices[ic].x, ctx.parcel_render_blob->vertices[ic].y, ctx.math_zoom)),
                                    fill);
                            }
                        }
                    }
                }
            }
        }
    }

    if (ctx.vacant_notice_enabled || ctx.vacant_rehab_enabled) {
        for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
            auto& fg = parcel_layer.features[i];
            if (!ctx.feature_passes_filters(ctx.parcel_layer_idx, i, fg)) continue;
            if (!featureOnScreen(ctx, ctx.parcel_layer_idx, (uint32_t)i, fg)) continue;
            if (!parcelOverlayHasGeometry(ctx, i, fg)) continue;

            const int vac_notice = valueAt(ctx.parcel_vac_notice_by_feature, i);
            const int vac_rehab = valueAt(ctx.parcel_vac_rehab_by_feature, i);
            const int weight = overlayWeight(ctx.vacant_notice_enabled, vac_notice, ctx.vacant_rehab_enabled, vac_rehab);
            if (weight <= 0) continue;
            result.visible_vacant_parcels++;

            const int alpha = scaledOverlayAlpha(120, 18, 120, 230, weight);
            const ImVec4 vac_base = blendVacancyColor(ctx.vacancy_notice_color, ctx.vacancy_rehab_color, vac_notice, vac_rehab);
            const ImU32 vac_fill = colorWithAlpha(vac_base, alpha);
            const ImU32 vac_outline = colorWithAlpha(darkenColor(vac_base, 0.62f), 235);
            const bool notice_fill = layerFillEnabled(ctx, ctx.vacant_notice_layer_idx);
            const bool rehab_fill = layerFillEnabled(ctx, ctx.vacant_rehab_layer_idx);
            if ((notice_fill || rehab_fill) && ctx.should_fill_layer_polygon(ctx.parcel_layer_idx) && !parcelGpuOverlayDrawActive()) {
                if (const ParcelRenderFeatureRecord* rec = parcelRenderFeature(ctx, i)) {
                    const uint32_t end = rec->index_offset + rec->index_count;
                    if (ctx.parcel_render_blob && end <= ctx.parcel_render_blob->indices.size()) {
                        for (uint32_t ti = rec->index_offset; ti + 2 < end; ti += 3) {
                            const uint32_t ia = ctx.parcel_render_blob->indices[ti];
                            const uint32_t ib = ctx.parcel_render_blob->indices[ti + 1];
                            const uint32_t ic = ctx.parcel_render_blob->indices[ti + 2];
                            if (ia >= ctx.parcel_render_blob->vertices.size() ||
                                ib >= ctx.parcel_render_blob->vertices.size() ||
                                ic >= ctx.parcel_render_blob->vertices.size()) continue;
                            ctx.draw->AddTriangleFilled(
                                ctx.projection->projectWorld(lonLatToWorldPx(ctx.parcel_render_blob->vertices[ia].x, ctx.parcel_render_blob->vertices[ia].y, ctx.math_zoom)),
                                ctx.projection->projectWorld(lonLatToWorldPx(ctx.parcel_render_blob->vertices[ib].x, ctx.parcel_render_blob->vertices[ib].y, ctx.math_zoom)),
                                ctx.projection->projectWorld(lonLatToWorldPx(ctx.parcel_render_blob->vertices[ic].x, ctx.parcel_render_blob->vertices[ic].y, ctx.math_zoom)),
                                vac_fill);
                        }
                    }
                }
            }
            static const std::vector<std::vector<ImVec2>> kEmptyWorldRings;
            drawParcelOverlayRings(ctx, i, fg, kEmptyWorldRings, vac_outline);
        }
    }

    if (ctx.tax_lien_enabled || ctx.tax_sale_enabled) {
        for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
            auto& fg = parcel_layer.features[i];
            if (!ctx.feature_passes_filters(ctx.parcel_layer_idx, i, fg)) continue;
            if (!featureOnScreen(ctx, ctx.parcel_layer_idx, (uint32_t)i, fg)) continue;
            if (!parcelOverlayHasGeometry(ctx, i, fg)) continue;

            const int lien_count = valueAt(ctx.parcel_tax_lien_by_feature, i);
            const int sale_count = valueAt(ctx.parcel_tax_sale_by_feature, i);
            const int weight = overlayWeight(ctx.tax_lien_enabled, lien_count, ctx.tax_sale_enabled, sale_count);
            if (weight <= 0) continue;

            const ImVec4 lien_c = (ctx.tax_lien_layer_idx >= 0) ? (*ctx.layers)[(size_t)ctx.tax_lien_layer_idx].color : ImVec4(0.95f, 0.55f, 0.1f, 1.0f);
            const ImVec4 sale_c = (ctx.tax_sale_layer_idx >= 0) ? (*ctx.layers)[(size_t)ctx.tax_sale_layer_idx].color : ImVec4(0.85f, 0.2f, 0.1f, 1.0f);
            const ImVec4 tax_base = blendTaxColor(lien_c, sale_c, ctx.tax_lien_enabled, ctx.tax_sale_enabled, lien_count, sale_count);
            const bool lien_fill = ctx.tax_lien_enabled && layerFillEnabled(ctx, ctx.tax_lien_layer_idx);
            const bool sale_fill = ctx.tax_sale_enabled && layerFillEnabled(ctx, ctx.tax_sale_layer_idx);
            if ((lien_fill || sale_fill) && ctx.should_fill_layer_polygon(ctx.parcel_layer_idx) && !parcelGpuOverlayDrawActive()) {
                const int alpha = scaledOverlayAlpha(90, 10, 90, 210, weight);
                const ImU32 fill = colorWithAlpha(tax_base, alpha);
                if (const ParcelRenderFeatureRecord* rec = parcelRenderFeature(ctx, i)) {
                    const uint32_t end = rec->index_offset + rec->index_count;
                    if (ctx.parcel_render_blob && end <= ctx.parcel_render_blob->indices.size()) {
                        for (uint32_t ti = rec->index_offset; ti + 2 < end; ti += 3) {
                            const uint32_t ia = ctx.parcel_render_blob->indices[ti];
                            const uint32_t ib = ctx.parcel_render_blob->indices[ti + 1];
                            const uint32_t ic = ctx.parcel_render_blob->indices[ti + 2];
                            if (ia >= ctx.parcel_render_blob->vertices.size() ||
                                ib >= ctx.parcel_render_blob->vertices.size() ||
                                ic >= ctx.parcel_render_blob->vertices.size()) continue;
                            ctx.draw->AddTriangleFilled(
                                ctx.projection->projectWorld(lonLatToWorldPx(ctx.parcel_render_blob->vertices[ia].x, ctx.parcel_render_blob->vertices[ia].y, ctx.math_zoom)),
                                ctx.projection->projectWorld(lonLatToWorldPx(ctx.parcel_render_blob->vertices[ib].x, ctx.parcel_render_blob->vertices[ib].y, ctx.math_zoom)),
                                ctx.projection->projectWorld(lonLatToWorldPx(ctx.parcel_render_blob->vertices[ic].x, ctx.parcel_render_blob->vertices[ic].y, ctx.math_zoom)),
                                fill);
                        }
                    }
                }
            }
            const ImU32 tax_outline = colorWithAlpha(darkenColor(tax_base, 0.58f), 240);
            static const std::vector<std::vector<ImVec2>> kEmptyWorldRings;
            drawParcelOverlayRings(ctx, i, fg, kEmptyWorldRings, tax_outline);
        }
    }

    return result;
}
