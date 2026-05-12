#include "render_layer_pass.h"

#include "aggregate_visualization_strategies.h"
#include "feature_props.h"
#include "geo.h"
#include "layer_geometry.h"
#include "map_render_utils.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>

namespace {

std::string firstProp(const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        std::string v = getPropertyValue(fg, k);
        if (!v.empty()) return v;
    }
    return {};
}

bool pointInWorldRings(const std::vector<std::vector<ImVec2>>& rings, float x, float y) {
    if (rings.empty() || !pointInRing(rings[0], x, y)) return false;
    for (size_t ri = 1; ri < rings.size(); ++ri) {
        if (pointInRing(rings[ri], x, y)) return false;
    }
    return true;
}

void addHeatSamplesForFeature(
    const RenderLayerPassContext& ctx,
    size_t sample_layer_idx,
    uint32_t feature_idx,
    const LayerDef::FeatureGeom& fg,
    const ImVec2& p0w,
    const ImVec2& p1w,
    ImU32 feature_c,
    float feature_sample_value,
    bool feature_heat_value_valid) {
    HeatSample base;
    base.lon = (fg.extent.min_lon + fg.extent.max_lon) * 0.5f;
    base.lat = (fg.extent.min_lat + fg.extent.max_lat) * 0.5f;
    base.color = ImGui::ColorConvertU32ToFloat4(feature_c);
    base.value = feature_sample_value;
    base.has_value = feature_heat_value_valid;
    base.prefer_gradient =
        sample_layer_idx < ctx.layer_heatmap_use_gradient->size()
            ? (*ctx.layer_heatmap_use_gradient)[sample_layer_idx]
            : true;
    resolveLayerHeatSettings(*ctx.heatmap_policy, sample_layer_idx, base);

    const int aggregate_algo = resolveLayerAggregateAlgo(*ctx.heatmap_policy, sample_layer_idx);
    const bool area_choropleth =
        aggregate_algo == kAggregateMedianChoropleth &&
        feature_heat_value_valid &&
        !fg.rings.empty();
    if (area_choropleth) {
        const auto& world_rings = ctx.projection->getWorldRings(sample_layer_idx, feature_idx, fg);
        const float cell = std::max(2.0f, base.cell_px);
        const float min_x = std::min(p0w.x, p1w.x);
        const float max_x = std::max(p0w.x, p1w.x);
        const float min_y = std::min(p0w.y, p1w.y);
        const float max_y = std::max(p0w.y, p1w.y);
        const int bx0 = (int)std::floor(min_x / cell);
        const int bx1 = (int)std::floor(max_x / cell);
        const int by0 = (int)std::floor(min_y / cell);
        const int by1 = (int)std::floor(max_y / cell);
        const size_t before = ctx.heat_samples->size();
        for (int by = by0; by <= by1; ++by) {
            const float cy = ((float)by + 0.5f) * cell;
            for (int bx = bx0; bx <= bx1; ++bx) {
                const float cx = ((float)bx + 0.5f) * cell;
                if (!pointInWorldRings(world_rings, cx, cy)) continue;
                HeatSample hs = base;
                hs.x = cx;
                hs.y = cy;
                ctx.heat_samples->push_back(hs);
            }
        }
        if (ctx.heat_samples->size() > before) return;
    }

    HeatSample hs = base;
    hs.x = (p0w.x + p1w.x) * 0.5f;
    hs.y = (p0w.y + p1w.y) * 0.5f;
    ctx.heat_samples->push_back(hs);
}

void drawFeatureGeometry(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureGeom& fg,
    ImU32 feature_c,
    bool layer_uses_lod_for_draw) {
    if (!fg.rings.empty()) {
        const auto& world_rings = ctx.projection->getWorldRings(layer_idx, (uint32_t)feature_idx, fg);
        const bool fill_enabled_for_layer =
            layer_idx < ctx.layer_fill_enabled->size() && (*ctx.layer_fill_enabled)[layer_idx];
        if (fill_enabled_for_layer && ctx.should_fill_layer_polygon(layer_idx)) {
            ImU32 fill = (feature_c & 0x00FFFFFF) | (170u << 24);
            ctx.projection->drawTessellatedFill(ctx.draw, fg, world_rings, fill);
        }

        if (layer_idx == (size_t)ctx.parcel_layer_idx &&
            (ctx.vacant_notice_overlay_enabled || ctx.vacant_rehab_overlay_enabled)) {
            int vac_notice = 0;
            int vac_rehab = 0;
            if (feature_idx < ctx.parcel_vac_notice_by_feature->size()) vac_notice = (*ctx.parcel_vac_notice_by_feature)[feature_idx];
            if (feature_idx < ctx.parcel_vac_rehab_by_feature->size()) vac_rehab = (*ctx.parcel_vac_rehab_by_feature)[feature_idx];

            const int weight = overlayWeight(
                ctx.vacant_notice_overlay_enabled,
                vac_notice,
                ctx.vacant_rehab_overlay_enabled,
                vac_rehab);
            if (weight > 0) {
                const bool notice_fill = ctx.vacant_notice_overlay_enabled &&
                    ctx.vacant_notice_layer_idx >= 0 &&
                    (size_t)ctx.vacant_notice_layer_idx < ctx.layer_fill_enabled->size() &&
                    (*ctx.layer_fill_enabled)[(size_t)ctx.vacant_notice_layer_idx];
                const bool rehab_fill = ctx.vacant_rehab_overlay_enabled &&
                    ctx.vacant_rehab_layer_idx >= 0 &&
                    (size_t)ctx.vacant_rehab_layer_idx < ctx.layer_fill_enabled->size() &&
                    (*ctx.layer_fill_enabled)[(size_t)ctx.vacant_rehab_layer_idx];
                if ((notice_fill || rehab_fill) && ctx.should_fill_layer_polygon((size_t)ctx.parcel_layer_idx)) {
                    const int alpha = scaledOverlayAlpha(95, 16, 95, 220, weight);
                    ImVec4 vac_base = blendVacancyColor(
                        *ctx.vacancy_notice_color,
                        *ctx.vacancy_rehab_color,
                        vac_notice,
                        vac_rehab);
                    ImU32 vac_fill = colorWithAlpha(vac_base, alpha);
                    ctx.projection->drawTessellatedFill(ctx.draw, fg, world_rings, vac_fill);
                }
            }
        }

        for (const auto& r : world_rings) {
            ctx.projection->appendWorldRingLine(r, layer_uses_lod_for_draw ? ctx.lod_ring_step : 1);
            const auto& line = ctx.projection->scratchLine();
            ctx.draw->AddPolyline(line.data(), (int)line.size(), feature_c, ImDrawFlags_Closed, 1.0f);
        }
        return;
    }

    ImVec2 pw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, ctx.math_zoom);
    ImVec2 ps = ctx.project_world(pw);
    if (ps.x >= ctx.origin.x && ps.x <= ctx.origin.x + ctx.size.x &&
        ps.y >= ctx.origin.y && ps.y <= ctx.origin.y + ctx.size.y) {
        float r = std::clamp((float)(2.0 * ctx.zoom_scale + 1.5), 2.0f, 6.0f);
        ctx.draw->AddCircleFilled(ps, r, feature_c);
        if (ctx.prof_features_drawn_frame) {
            ++(*ctx.prof_features_drawn_frame);
        }
    }
}

void renderFeature(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef& layer,
    const LayerDef::FeatureGeom& fg,
    ImU32 base_color,
    bool is_heat_layer,
    bool is_zoning_layer,
    const HeatNormalizationState& heat_normalization,
    const std::function<std::string(const LayerDef::FeatureGeom&)>& normalization_group_key,
    bool layer_uses_heatmap,
    bool layer_uses_lod_for_draw,
    bool apply_smooth_stride,
    size_t smooth_sample_stride) {
    if (ctx.prof_features_considered_frame) {
        ++(*ctx.prof_features_considered_frame);
    }
    if (!ctx.feature_passes_filters(layer_idx, feature_idx, fg)) return;

    ImU32 feature_c = base_color;
    float feature_heat_value = 0.0f;
    float feature_normalized_value = 0.0f;
    bool feature_heat_value_valid = false;
    if (is_heat_layer) {
        feature_heat_value_valid = tryGetFeaturePropertyFloat(fg, layer.heatmap_field, feature_heat_value);
        if (feature_heat_value_valid &&
            heat_normalization.normalizedValue(fg, feature_heat_value, normalization_group_key, feature_normalized_value)) {
            feature_c = ImGui::ColorConvertFloat4ToU32(heatColor(feature_normalized_value));
        }
    }
    if (is_zoning_layer) {
        const std::string zkey = zoningClassKey(fg);
        auto it_en = ctx.zoning_zone_enabled->find(zkey);
        if (it_en != ctx.zoning_zone_enabled->end() && !it_en->second) return;
        auto it_col = ctx.zoning_zone_color->find(zkey);
        if (it_col != ctx.zoning_zone_color->end()) feature_c = ImGui::ColorConvertFloat4ToU32(it_col->second);
    }
    ctx.query_map_color(layer_idx, feature_idx, fg, feature_c);

    const auto& pww = ctx.projection->getWorldExtent(layer_idx, (uint32_t)feature_idx, fg);
    ImVec2 p0w = pww.first;
    ImVec2 p1w = pww.second;
    ImVec2 a = ctx.project_world(p0w);
    ImVec2 b = ctx.project_world(p1w);
    ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
    ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
    if (!layer_uses_heatmap &&
        (p1.x < ctx.origin.x || p0.x > ctx.origin.x + ctx.size.x || p1.y < ctx.origin.y || p0.y > ctx.origin.y + ctx.size.y)) {
        return;
    }
    if (layer_uses_heatmap) {
        if (ctx.can_use_cached_heatmap) return;
        if (apply_smooth_stride && smooth_sample_stride > 1 && (feature_idx % smooth_sample_stride) != 0) return;
        addHeatSamplesForFeature(
            ctx,
            layer_idx,
            (uint32_t)feature_idx,
            fg,
            p0w,
            p1w,
            feature_c,
            heat_normalization.normalize_mode == 0 ? feature_heat_value : feature_normalized_value,
            feature_heat_value_valid);
        return;
    }

    if (p1.x < ctx.origin.x || p0.x > ctx.origin.x + ctx.size.x || p1.y < ctx.origin.y || p0.y > ctx.origin.y + ctx.size.y) {
        return;
    }
    drawFeatureGeometry(ctx, layer_idx, feature_idx, fg, feature_c, layer_uses_lod_for_draw);
}

} // namespace

void runRenderLayerPass(const RenderLayerPassContext& ctx) {
    std::vector<uint32_t> render_candidates;
    for (size_t layer_idx : ctx.render_plan->draw_layer_order) {
        auto& l = (*ctx.layers)[layer_idx];
        if (!l.enabled) continue;

        const bool layer_uses_heatmap_for_cache = layerUsesHeatmapAggregate(*ctx.heatmap_policy, layer_idx);
        const bool layer_uses_lod_for_draw = layerUsesLodGeometry(*ctx.heatmap_policy, layer_idx);
        if (shouldSkipRawSourceLayer(layer_idx, ctx.raw_source_layer_policy) &&
            !layer_uses_heatmap_for_cache &&
            !layer_uses_lod_for_draw) {
            continue;
        }

        const bool is_zoning_layer = (int)layer_idx == ctx.zoning_layer_idx;
        const bool is_heat_layer = !l.heatmap_field.empty();
        auto normalization_group_key = [&](const LayerDef::FeatureGeom& fg) {
            std::string key = firstProp(fg, {
                "ZONECODE", "ZONING", "ZONE", "zoning", "zoning_group",
                "group_key", "LANDUSE", "LAND_USE", "USE", "CATEGORY", "category"
            });
            return normalizeJoinKey(key);
        };
        const HeatNormalizationState heat_normalization =
            is_heat_layer
                ? buildHeatNormalizationState(
                    l,
                    layer_idx,
                    *ctx.layer_normalize_mode,
                    *ctx.layer_heatmap_percentile_clip,
                    ctx.heatmap_policy->heatmap_percentile_clip,
                    ctx.feature_passes_filters,
                    normalization_group_key)
                : HeatNormalizationState{};

        ImU32 base_color = ImGui::ColorConvertFloat4ToU32(l.color);
        bool have_candidates = !ctx.should_recompute_heatmap || !layer_uses_heatmap_for_cache;
        if (ctx.high_quality_gpu_aggregate && ctx.should_recompute_heatmap && layer_uses_heatmap_for_cache) {
            have_candidates = false;
        }
        if (have_candidates) {
            have_candidates = queryLayerSpatialIndex(
                (*ctx.layer_spatial)[layer_idx],
                ctx.view_min_lon,
                ctx.view_min_lat,
                ctx.view_max_lon,
                ctx.view_max_lat,
                render_candidates);
        }
        if (have_candidates) {
            for (uint32_t fidx : render_candidates) {
                const bool layer_uses_heatmap = layerUsesHeatmapAggregate(*ctx.heatmap_policy, layer_idx);
                if (!ctx.high_quality_gpu_aggregate &&
                    ctx.smooth_only_heatmap &&
                    ctx.should_recompute_heatmap &&
                    layer_uses_heatmap &&
                    fidx % 2 != 0) {
                    continue;
                }
                if (fidx >= l.features.size()) continue;
                auto& fg = l.features[(size_t)fidx];
                renderFeature(
                    ctx,
                    layer_idx,
                    (size_t)fidx,
                    l,
                    fg,
                    base_color,
                    is_heat_layer,
                    is_zoning_layer,
                    heat_normalization,
                    normalization_group_key,
                    layer_uses_heatmap,
                    layer_uses_lod_for_draw,
                    false,
                    1);
            }
            continue;
        }

        const size_t smooth_sample_stride =
            (!ctx.high_quality_gpu_aggregate &&
             ctx.smooth_only_heatmap &&
             ctx.should_recompute_heatmap &&
             layer_uses_heatmap_for_cache &&
             l.features.size() > kMaxSmoothHeatSamplesPerLayer)
                ? std::max<size_t>(1, (l.features.size() + kMaxSmoothHeatSamplesPerLayer - 1) / kMaxSmoothHeatSamplesPerLayer)
                : 1;
        for (size_t fi = 0; fi < l.features.size(); ++fi) {
            auto& fg = l.features[fi];
            const bool layer_uses_heatmap = layerUsesHeatmapAggregate(*ctx.heatmap_policy, layer_idx);
            renderFeature(
                ctx,
                layer_idx,
                fi,
                l,
                fg,
                base_color,
                is_heat_layer,
                is_zoning_layer,
                heat_normalization,
                normalization_group_key,
                layer_uses_heatmap,
                layer_uses_lod_for_draw,
                true,
                smooth_sample_stride);
        }
    }
}
