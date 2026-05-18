#include "render_layer_pass.h"

#include "aggregate_visualization_strategies.h"
#include "feature_props.h"
#include "geo.h"
#include "layer_geometry.h"
#include "map_render_utils.h"
#include "worldsim_app.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>

namespace {

constexpr size_t kMaxFallbackFullScanFeatures = 12000;
constexpr size_t kFallbackScanBudgetPerFrame = 4096;
constexpr size_t kMaxLargeParcelCpuFallbackFeatures = 100000;
constexpr float kPointMarkerRadiusPx = 5.0f;
constexpr float kPointMarkerOutlinePx = 1.6f;
constexpr int kPointClusterMaxMathZoom = 15;
constexpr float kPointClusterCellPx = 28.0f;
constexpr float kPointClusterRadiusPx = 12.0f;

enum class PointMarkerGlyph {
    Circle,
    Square,
    Diamond,
    Triangle,
    Plus,
    Cross,
    Droplet
};

struct PointClusterCellKey {
    int x = 0;
    int y = 0;
    bool operator==(const PointClusterCellKey& other) const {
        return x == other.x && y == other.y;
    }
};

struct PointClusterCellKeyHash {
    size_t operator()(const PointClusterCellKey& key) const {
        return (uint64_t(uint32_t(key.x)) << 32) ^ uint32_t(key.y);
    }
};

struct PointClusterBucket {
    ImVec2 center_sum = ImVec2(0.0f, 0.0f);
    ImU32 color = 0;
    PointMarkerGlyph glyph = PointMarkerGlyph::Circle;
    size_t count = 0;
    float min_lon = 0.0f;
    float max_lon = 0.0f;
    float min_lat = 0.0f;
    float max_lat = 0.0f;
};

uint64_t heatNormalizationCacheKey(uint64_t heatmap_data_key, size_t layer_idx) {
    return (heatmap_data_key * 1099511628211ULL) ^ (uint64_t(layer_idx) + 0x9e3779b97f4a7c15ULL);
}

uint64_t mixStyleHash(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

uint64_t hashImU32(ImU32 color) {
    return uint64_t(color) * 11400714819323198485ull;
}

uint64_t hashZoningStyleState(
    const std::unordered_map<std::string, bool>& zoning_zone_enabled,
    const std::unordered_map<std::string, ImVec4>& zoning_zone_color) {
    uint64_t h = 1469598103934665603ull;
    std::vector<std::string> keys;
    keys.reserve(zoning_zone_color.size() + zoning_zone_enabled.size());
    for (const auto& kv : zoning_zone_color) keys.push_back(kv.first);
    for (const auto& kv : zoning_zone_enabled) {
        if (zoning_zone_color.find(kv.first) == zoning_zone_color.end()) keys.push_back(kv.first);
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    for (const std::string& key : keys) {
        for (unsigned char ch : key) h = mixStyleHash(h, uint64_t(ch));
        auto en_it = zoning_zone_enabled.find(key);
        auto col_it = zoning_zone_color.find(key);
        h = mixStyleHash(h, en_it != zoning_zone_enabled.end() && en_it->second ? 1ull : 0ull);
        if (col_it != zoning_zone_color.end()) {
            h = mixStyleHash(h, hashImU32(ImGui::ColorConvertFloat4ToU32(col_it->second)));
        }
    }
    return h;
}

uint64_t featureStyleKey(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    ImU32 base_color,
    bool is_heat_layer,
    bool is_zoning_layer) {
    uint64_t key = mixStyleHash(1469598103934665603ull, uint64_t(layer_idx));
    key = mixStyleHash(key, ctx.heatmap_data_key);
    key = mixStyleHash(key, hashImU32(base_color));
    if (is_heat_layer) key = mixStyleHash(key, 0x48454154ull);
    if (is_zoning_layer && ctx.zoning_zone_enabled && ctx.zoning_zone_color) {
        key = mixStyleHash(key, hashZoningStyleState(*ctx.zoning_zone_enabled, *ctx.zoning_zone_color));
    }
    return key;
}

ImU32 computeFeatureColor(
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
    float& feature_heat_value,
    float& feature_normalized_value,
    bool& feature_heat_value_valid) {
    ImU32 feature_c = base_color;
    feature_heat_value = 0.0f;
    feature_normalized_value = 0.0f;
    feature_heat_value_valid = false;
    if (is_heat_layer) {
        feature_heat_value_valid = tryGetFeaturePropertyFloat(fg, layer.heatmap_field, feature_heat_value);
        if (feature_heat_value_valid &&
            heat_normalization.normalizedValue(fg, feature_heat_value, normalization_group_key, feature_normalized_value)) {
            const float gamma =
                ctx.layer_choropleth_gamma && layer_idx < ctx.layer_choropleth_gamma->size()
                    ? (*ctx.layer_choropleth_gamma)[layer_idx]
                    : 1.0f;
            feature_normalized_value = applyPowerGamma(feature_normalized_value, gamma);
            feature_c = ImGui::ColorConvertFloat4ToU32(heatColor(feature_normalized_value));
        }
    }
    if (is_zoning_layer) {
        const std::string zkey = zoningClassKey(fg);
        auto it_col = ctx.zoning_zone_color->find(zkey);
        if (it_col != ctx.zoning_zone_color->end()) feature_c = ImGui::ColorConvertFloat4ToU32(it_col->second);
    }
    ctx.query_map_color(layer_idx, feature_idx, fg, feature_c);
    return feature_c;
}

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

bool containsCaseInsensitive(const std::string& haystack, const char* needle) {
    if (!needle || !*needle) return false;
    std::string hs = haystack;
    std::string nd = needle;
    std::transform(hs.begin(), hs.end(), hs.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(nd.begin(), nd.end(), nd.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return hs.find(nd) != std::string::npos;
}

PointMarkerGlyph pointMarkerGlyphForLayer(const LayerDef& layer) {
    if (containsCaseInsensitive(layer.name, "water")) return PointMarkerGlyph::Droplet;
    if (containsCaseInsensitive(layer.name, "health")) return PointMarkerGlyph::Cross;
    if (containsCaseInsensitive(layer.name, "school")) return PointMarkerGlyph::Triangle;
    if (containsCaseInsensitive(layer.name, "market")) return PointMarkerGlyph::Diamond;
    if (containsCaseInsensitive(layer.name, "police")) return PointMarkerGlyph::Diamond;
    if (containsCaseInsensitive(layer.name, "church")) return PointMarkerGlyph::Plus;
    if (containsCaseInsensitive(layer.name, "industry")) return PointMarkerGlyph::Square;
    if (containsCaseInsensitive(layer.name, "filling")) return PointMarkerGlyph::Square;
    switch (layer.category) {
        case LayerDef::Category::PublicHealth: return PointMarkerGlyph::Cross;
        case LayerDef::Category::Infrastructure: return PointMarkerGlyph::Square;
        case LayerDef::Category::Safety: return PointMarkerGlyph::Diamond;
        case LayerDef::Category::Zoning: return PointMarkerGlyph::Triangle;
        case LayerDef::Category::Housing:
        default: return PointMarkerGlyph::Circle;
    }
}

void drawPointMarker(
    ImDrawList* draw,
    const ImVec2& center,
    ImU32 fill_color,
    PointMarkerGlyph glyph,
    float radius_px) {
    const ImU32 outline = IM_COL32(18, 22, 26, 235);
    const float r = radius_px;
    const float ro = r + kPointMarkerOutlinePx;
    switch (glyph) {
        case PointMarkerGlyph::Square: {
            draw->AddRectFilled(ImVec2(center.x - ro, center.y - ro), ImVec2(center.x + ro, center.y + ro), outline, 2.0f);
            draw->AddRectFilled(ImVec2(center.x - r, center.y - r), ImVec2(center.x + r, center.y + r), fill_color, 1.6f);
            break;
        }
        case PointMarkerGlyph::Diamond: {
            ImVec2 o[4] = {
                ImVec2(center.x, center.y - ro),
                ImVec2(center.x + ro, center.y),
                ImVec2(center.x, center.y + ro),
                ImVec2(center.x - ro, center.y)
            };
            ImVec2 i[4] = {
                ImVec2(center.x, center.y - r),
                ImVec2(center.x + r, center.y),
                ImVec2(center.x, center.y + r),
                ImVec2(center.x - r, center.y)
            };
            draw->AddConvexPolyFilled(o, 4, outline);
            draw->AddConvexPolyFilled(i, 4, fill_color);
            break;
        }
        case PointMarkerGlyph::Triangle: {
            ImVec2 o[3] = {
                ImVec2(center.x, center.y - ro),
                ImVec2(center.x + ro * 0.92f, center.y + ro * 0.82f),
                ImVec2(center.x - ro * 0.92f, center.y + ro * 0.82f)
            };
            ImVec2 i[3] = {
                ImVec2(center.x, center.y - r),
                ImVec2(center.x + r * 0.92f, center.y + r * 0.82f),
                ImVec2(center.x - r * 0.92f, center.y + r * 0.82f)
            };
            draw->AddConvexPolyFilled(o, 3, outline);
            draw->AddConvexPolyFilled(i, 3, fill_color);
            break;
        }
        case PointMarkerGlyph::Plus: {
            draw->AddCircleFilled(center, ro, outline, 18);
            draw->AddCircleFilled(center, r, fill_color, 18);
            const float arm_o = ro * 0.72f;
            const float arm_i = r * 0.72f;
            draw->AddLine(ImVec2(center.x - arm_o, center.y), ImVec2(center.x + arm_o, center.y), outline, 2.8f);
            draw->AddLine(ImVec2(center.x, center.y - arm_o), ImVec2(center.x, center.y + arm_o), outline, 2.8f);
            draw->AddLine(ImVec2(center.x - arm_i, center.y), ImVec2(center.x + arm_i, center.y), IM_COL32(245, 248, 250, 235), 1.6f);
            draw->AddLine(ImVec2(center.x, center.y - arm_i), ImVec2(center.x, center.y + arm_i), IM_COL32(245, 248, 250, 235), 1.6f);
            break;
        }
        case PointMarkerGlyph::Cross: {
            draw->AddCircleFilled(center, ro, outline, 18);
            draw->AddCircleFilled(center, r, fill_color, 18);
            const float arm_o = ro * 0.62f;
            const float arm_i = r * 0.62f;
            draw->AddLine(ImVec2(center.x - arm_o, center.y - arm_o), ImVec2(center.x + arm_o, center.y + arm_o), outline, 2.8f);
            draw->AddLine(ImVec2(center.x - arm_o, center.y + arm_o), ImVec2(center.x + arm_o, center.y - arm_o), outline, 2.8f);
            draw->AddLine(ImVec2(center.x - arm_i, center.y - arm_i), ImVec2(center.x + arm_i, center.y + arm_i), IM_COL32(245, 248, 250, 235), 1.6f);
            draw->AddLine(ImVec2(center.x - arm_i, center.y + arm_i), ImVec2(center.x + arm_i, center.y - arm_i), IM_COL32(245, 248, 250, 235), 1.6f);
            break;
        }
        case PointMarkerGlyph::Droplet: {
            ImVec2 o[3] = {
                ImVec2(center.x, center.y - ro - 1.0f),
                ImVec2(center.x + ro * 0.72f, center.y),
                ImVec2(center.x - ro * 0.72f, center.y)
            };
            ImVec2 i[3] = {
                ImVec2(center.x, center.y - r - 1.0f),
                ImVec2(center.x + r * 0.72f, center.y),
                ImVec2(center.x - r * 0.72f, center.y)
            };
            draw->AddConvexPolyFilled(o, 3, outline);
            draw->AddCircleFilled(ImVec2(center.x, center.y + ro * 0.28f), ro * 0.86f, outline, 18);
            draw->AddConvexPolyFilled(i, 3, fill_color);
            draw->AddCircleFilled(ImVec2(center.x, center.y + r * 0.28f), r * 0.86f, fill_color, 18);
            break;
        }
        case PointMarkerGlyph::Circle:
        default:
            draw->AddCircleFilled(center, ro, outline, 18);
            draw->AddCircleFilled(center, r, fill_color, 18);
            break;
    }
}

void drawPointClusterBadge(
    ImDrawList* draw,
    const ImVec2& center,
    ImU32 fill_color,
    size_t count) {
    const ImU32 outline = IM_COL32(18, 22, 26, 235);
    draw->AddCircleFilled(center, kPointClusterRadiusPx + 2.0f, outline, 24);
    draw->AddCircleFilled(center, kPointClusterRadiusPx, fill_color, 24);
    const std::string label = std::to_string(count);
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    draw->AddText(
        ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f - 0.5f),
        IM_COL32(245, 248, 250, 245),
        label.c_str());
}

bool pointInsideCircle(const ImVec2& p, const ImVec2& center, float radius) {
    const float dx = p.x - center.x;
    const float dy = p.y - center.y;
    return dx * dx + dy * dy <= radius * radius;
}

bool shouldClusterPointLayer(
    const RenderLayerPassContext& ctx,
    const LayerDef& layer,
    bool layer_uses_heatmap,
    bool layer_uses_lod_for_draw) {
    if (layer_uses_heatmap || layer_uses_lod_for_draw) return false;
    if (ctx.math_zoom > kPointClusterMaxMathZoom) return false;
    return layer.scale == "point";
}

bool resolveFeatureRenderStyle(
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
    ImU32& feature_c,
    float& feature_heat_value,
    float& feature_normalized_value,
    bool& feature_heat_value_valid) {
    if (!ctx.feature_passes_filters(layer_idx, feature_idx, fg)) return false;
    if (is_zoning_layer) {
        const std::string zkey = zoningClassKey(fg);
        auto it_en = ctx.zoning_zone_enabled->find(zkey);
        if (it_en != ctx.zoning_zone_enabled->end() && !it_en->second) return false;
    }
    const uint64_t style_key = featureStyleKey(ctx, layer_idx, base_color, is_heat_layer, is_zoning_layer);
    const CachedFeatureColorStorage* cached_colors =
        ctx.projection->findFeatureColorStorage(layer_idx, (uint32_t)feature_idx, style_key);
    if (cached_colors) {
        feature_c = cached_colors->feature_color;
        feature_heat_value = 0.0f;
        feature_normalized_value = 0.0f;
        feature_heat_value_valid = false;
        if (is_heat_layer) {
            feature_heat_value_valid = tryGetFeaturePropertyFloat(fg, layer.heatmap_field, feature_heat_value);
            if (feature_heat_value_valid &&
                heat_normalization.normalizedValue(fg, feature_heat_value, normalization_group_key, feature_normalized_value)) {
                const float gamma =
                    ctx.layer_choropleth_gamma && layer_idx < ctx.layer_choropleth_gamma->size()
                        ? (*ctx.layer_choropleth_gamma)[layer_idx]
                        : 1.0f;
                feature_normalized_value = applyPowerGamma(feature_normalized_value, gamma);
            }
        }
        return true;
    }

    feature_c = computeFeatureColor(
        ctx,
        layer_idx,
        feature_idx,
        layer,
        fg,
        base_color,
        is_heat_layer,
        is_zoning_layer,
        heat_normalization,
        normalization_group_key,
        feature_heat_value,
        feature_normalized_value,
        feature_heat_value_valid);
    ctx.projection->storeFeatureColorStorage(
        layer_idx,
        (uint32_t)feature_idx,
        style_key,
        feature_c,
        fg.rings.empty() ? 0 : 1);
    return true;
}

bool projectFeatureScreenBounds(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureGeom& fg,
    ImVec2& p0w,
    ImVec2& p1w,
    ImVec2& p0,
    ImVec2& p1) {
    const auto& pww = ctx.projection->getWorldExtent(layer_idx, (uint32_t)feature_idx, fg);
    p0w = pww.first;
    p1w = pww.second;
    ImVec2 a = ctx.project_world(p0w);
    ImVec2 b = ctx.project_world(p1w);
    p0 = ImVec2(std::min(a.x, b.x), std::min(a.y, b.y));
    p1 = ImVec2(std::max(a.x, b.x), std::max(a.y, b.y));
    return !(p1.x < ctx.origin.x || p0.x > ctx.origin.x + ctx.size.x || p1.y < ctx.origin.y || p0.y > ctx.origin.y + ctx.size.y);
}

void renderClusteredPointCandidates(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    const LayerDef& layer,
    const std::vector<uint32_t>& feature_indices,
    ImU32 base_color,
    bool is_heat_layer,
    bool is_zoning_layer,
    const HeatNormalizationState& heat_normalization,
    const std::function<std::string(const LayerDef::FeatureGeom&)>& normalization_group_key) {
    std::unordered_map<PointClusterCellKey, PointClusterBucket, PointClusterCellKeyHash> buckets;
    buckets.reserve(feature_indices.size());
    for (uint32_t fidx : feature_indices) {
        if ((size_t)fidx >= layer.features.size()) continue;
        const auto& fg = layer.features[(size_t)fidx];
        if (!fg.rings.empty()) continue;
        ImU32 feature_c = base_color;
        float feature_heat_value = 0.0f;
        float feature_normalized_value = 0.0f;
        bool feature_heat_value_valid = false;
        if (!resolveFeatureRenderStyle(
                ctx,
                layer_idx,
                (size_t)fidx,
                layer,
                fg,
                base_color,
                is_heat_layer,
                is_zoning_layer,
                heat_normalization,
                normalization_group_key,
                feature_c,
                feature_heat_value,
                feature_normalized_value,
                feature_heat_value_valid)) {
            continue;
        }
        ImVec2 p0w, p1w, p0, p1;
        if (!projectFeatureScreenBounds(ctx, layer_idx, (size_t)fidx, fg, p0w, p1w, p0, p1)) continue;
        ImVec2 pw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, ctx.math_zoom);
        ImVec2 ps = ctx.project_world(pw);
        const PointClusterCellKey key{
            (int)std::floor((ps.x - ctx.origin.x) / kPointClusterCellPx),
            (int)std::floor((ps.y - ctx.origin.y) / kPointClusterCellPx)
        };
        auto& bucket = buckets[key];
        bucket.center_sum.x += ps.x;
        bucket.center_sum.y += ps.y;
        if (bucket.count == 0) {
            bucket.color = feature_c;
            bucket.glyph = pointMarkerGlyphForLayer(layer);
            bucket.min_lon = fg.extent.min_lon;
            bucket.max_lon = fg.extent.max_lon;
            bucket.min_lat = fg.extent.min_lat;
            bucket.max_lat = fg.extent.max_lat;
        } else {
            bucket.min_lon = std::min(bucket.min_lon, fg.extent.min_lon);
            bucket.max_lon = std::max(bucket.max_lon, fg.extent.max_lon);
            bucket.min_lat = std::min(bucket.min_lat, fg.extent.min_lat);
            bucket.max_lat = std::max(bucket.max_lat, fg.extent.max_lat);
        }
        ++bucket.count;
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (const auto& kv : buckets) {
        const PointClusterBucket& bucket = kv.second;
        if (bucket.count == 0) continue;
        const ImVec2 center(bucket.center_sum.x / (float)bucket.count, bucket.center_sum.y / (float)bucket.count);
        if (bucket.count == 1) {
            drawPointMarker(ctx.draw, center, bucket.color, bucket.glyph, kPointMarkerRadiusPx);
        } else {
            drawPointClusterBadge(ctx.draw, center, bucket.color, bucket.count);
            if (pointInsideCircle(mouse, center, kPointClusterRadiusPx + 3.0f)) {
                ImGui::BeginTooltip();
                ImGui::Text("%s", layer.name.c_str());
                ImGui::Separator();
                ImGui::Text("%zu locations", bucket.count);
                ImGui::TextDisabled("Click to zoom in");
                ImGui::EndTooltip();
                if (ctx.center_lon && ctx.center_lat && ctx.zoom && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    *ctx.center_lon = 0.5 * ((double)bucket.min_lon + (double)bucket.max_lon);
                    *ctx.center_lat = 0.5 * ((double)bucket.min_lat + (double)bucket.max_lat);
                    *ctx.zoom = std::min(ctx.max_zoom, std::max(*ctx.zoom + 2, ctx.math_zoom + 1));
                }
            }
        }
        if (ctx.prof_features_drawn_frame) {
            ++(*ctx.prof_features_drawn_frame);
        }
    }
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
    const LayerDef& layer,
    const LayerDef::FeatureGeom& fg,
    ImU32 feature_c,
    bool layer_uses_lod_for_draw) {
    if (!fg.rings.empty()) {
        const auto& world_rings = ctx.projection->getWorldRings(layer_idx, (uint32_t)feature_idx, fg);
        const bool fill_enabled_for_layer =
            layer_idx < ctx.layer_fill_enabled->size() && (*ctx.layer_fill_enabled)[layer_idx];
        const bool use_gpu_parcel_fill =
            (int)layer_idx == ctx.parcel_layer_idx && parcelGpuDrawActive();
        if (!use_gpu_parcel_fill && fill_enabled_for_layer && ctx.should_fill_layer_polygon(layer_idx)) {
            const uint32_t src_alpha = (feature_c >> 24) & 0xFFu;
            const uint32_t fill_alpha = (uint32_t)std::clamp(
                (int)std::lround((float)src_alpha * std::clamp(ctx.map_polygon_fill_opacity, 0.0f, 1.0f)),
                0,
                255);
            ImU32 fill = (feature_c & 0x00FFFFFF) | (fill_alpha << 24);
            ctx.projection->drawTessellatedFill(ctx.draw, layer_idx, (uint32_t)feature_idx, fg, fill);
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
                if ((notice_fill || rehab_fill) &&
                    ctx.should_fill_layer_polygon((size_t)ctx.parcel_layer_idx) &&
                    !parcelGpuOverlayDrawActive()) {
                    const int alpha = scaledOverlayAlpha(95, 16, 95, 220, weight);
                    ImVec4 vac_base = blendVacancyColor(
                        *ctx.vacancy_notice_color,
                        *ctx.vacancy_rehab_color,
                        vac_notice,
                        vac_rehab);
                    ImU32 vac_fill = colorWithAlpha(vac_base, alpha);
                    ctx.projection->drawTessellatedFill(ctx.draw, layer_idx, (uint32_t)feature_idx, fg, vac_fill);
                }
            }
        }

        for (const auto& r : world_rings) {
            if ((int)layer_idx != ctx.parcel_layer_idx || !parcelGpuOutlineDrawActive()) {
                ctx.projection->appendWorldRingLine(r, layer_uses_lod_for_draw ? ctx.lod_ring_step : 1);
                const auto& line = ctx.projection->scratchLine();
                ctx.draw->AddPolyline(line.data(), (int)line.size(), feature_c, ImDrawFlags_Closed, 1.0f);
            }
        }
        return;
    }

    ImVec2 pw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, ctx.math_zoom);
    ImVec2 ps = ctx.project_world(pw);
    if (ps.x >= ctx.origin.x && ps.x <= ctx.origin.x + ctx.size.x &&
        ps.y >= ctx.origin.y && ps.y <= ctx.origin.y + ctx.size.y) {
        drawPointMarker(ctx.draw, ps, feature_c, pointMarkerGlyphForLayer(layer), kPointMarkerRadiusPx);
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
    ImU32 feature_c = base_color;
    float feature_heat_value = 0.0f;
    float feature_normalized_value = 0.0f;
    bool feature_heat_value_valid = false;
    if (!resolveFeatureRenderStyle(
            ctx,
            layer_idx,
            feature_idx,
            layer,
            fg,
            base_color,
            is_heat_layer,
            is_zoning_layer,
            heat_normalization,
            normalization_group_key,
            feature_c,
            feature_heat_value,
            feature_normalized_value,
            feature_heat_value_valid)) {
        return;
    }

    ImVec2 p0w, p1w, p0, p1;
    const bool on_screen = projectFeatureScreenBounds(ctx, layer_idx, feature_idx, fg, p0w, p1w, p0, p1);
    if (!layer_uses_heatmap && !on_screen) return;
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

    drawFeatureGeometry(ctx, layer_idx, feature_idx, layer, fg, feature_c, layer_uses_lod_for_draw);
}

} // namespace

bool shouldBypassCpuParcelFeaturePass(
    bool parcel_gpu_draw_active,
    bool layer_uses_heatmap_for_cache,
    bool layer_uses_lod_for_draw,
    bool should_recompute_heatmap) {
    if (!parcel_gpu_draw_active) return false;
    if (layer_uses_lod_for_draw) return false;
    if (layer_uses_heatmap_for_cache && should_recompute_heatmap) return false;
    return true;
}

void runRenderLayerPass(const RenderLayerPassContext& ctx) {
    std::vector<uint32_t> render_candidates;
    for (size_t layer_idx : ctx.render_plan->draw_layer_order) {
        auto& l = (*ctx.layers)[layer_idx];
        if (!l.enabled) continue;
        const bool layer_uses_heatmap_for_cache = layerUsesHeatmapAggregate(*ctx.heatmap_policy, layer_idx);
        const bool layer_uses_lod_for_draw = layerUsesLodGeometry(*ctx.heatmap_policy, layer_idx);
        const bool use_gpu_parcel_draw = (int)layer_idx == ctx.parcel_layer_idx && parcelGpuDrawActive();
        if (use_gpu_parcel_draw) {
            enqueueParcelGpuDraw(ctx.draw);
        }
        if ((int)layer_idx == ctx.parcel_layer_idx &&
            shouldBypassCpuParcelFeaturePass(
                use_gpu_parcel_draw,
                layer_uses_heatmap_for_cache,
                layer_uses_lod_for_draw,
                ctx.should_recompute_heatmap)) {
            continue;
        }
        if ((int)layer_idx == ctx.parcel_layer_idx &&
            !use_gpu_parcel_draw &&
            l.features.size() > kMaxLargeParcelCpuFallbackFeatures) {
            continue;
        }

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
        HeatNormalizationState heat_normalization;
        if (is_heat_layer) {
            HeatNormalizationState* cached_heat_normalization = nullptr;
            if (ctx.heatmap_runtime) {
                auto& normalization_cache = ctx.heatmap_runtime->normalization_cache;
                const uint64_t cache_key = heatNormalizationCacheKey(ctx.heatmap_data_key, layer_idx);
                auto it = normalization_cache.find(cache_key);
                if (it == normalization_cache.end()) {
                    CachedHeatNormalization cached;
                    cached.state = buildHeatNormalizationState(
                        l,
                        layer_idx,
                        *ctx.layer_normalize_mode,
                        *ctx.layer_heatmap_percentile_clip,
                        ctx.heatmap_policy->heatmap_percentile_clip,
                        ctx.feature_passes_filters,
                        normalization_group_key);
                    cached.last_used_frame = ++ctx.heatmap_runtime->texture_cache_frame;
                    it = normalization_cache.emplace(cache_key, std::move(cached)).first;
                    while (normalization_cache.size() > 16) {
                        auto evict_it = normalization_cache.begin();
                        for (auto prune_it = normalization_cache.begin(); prune_it != normalization_cache.end(); ++prune_it) {
                            if (prune_it->second.last_used_frame < evict_it->second.last_used_frame) {
                                evict_it = prune_it;
                            }
                        }
                        normalization_cache.erase(evict_it);
                    }
                }
                it->second.last_used_frame = ++ctx.heatmap_runtime->texture_cache_frame;
                cached_heat_normalization = &it->second.state;
            }
            if (cached_heat_normalization) {
                heat_normalization = *cached_heat_normalization;
            } else {
                heat_normalization = buildHeatNormalizationState(
                    l,
                    layer_idx,
                    *ctx.layer_normalize_mode,
                    *ctx.layer_heatmap_percentile_clip,
                    ctx.heatmap_policy->heatmap_percentile_clip,
                    ctx.feature_passes_filters,
                    normalization_group_key);
            }
        }

        ImU32 base_color = ImGui::ColorConvertFloat4ToU32(l.color);
        const bool should_cluster_point_layer =
            shouldClusterPointLayer(ctx, l, is_heat_layer, layer_uses_lod_for_draw);
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
            if (should_cluster_point_layer) {
                renderClusteredPointCandidates(
                    ctx,
                    layer_idx,
                    l,
                    render_candidates,
                    base_color,
                    is_heat_layer,
                    is_zoning_layer,
                    heat_normalization,
                    normalization_group_key);
                continue;
            }
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
        const bool use_bounded_fallback_scan =
            ctx.layer_fallback_scan_cursor &&
            layer_idx < ctx.layer_fallback_scan_cursor->size() &&
            l.features.size() > kMaxFallbackFullScanFeatures;
        if (use_bounded_fallback_scan) {
            if (ctx.should_recompute_heatmap && layer_uses_heatmap_for_cache) {
                continue;
            }
            const size_t total = l.features.size();
            const size_t cursor = (*ctx.layer_fallback_scan_cursor)[layer_idx] % total;
            const size_t budget = std::min(total, kFallbackScanBudgetPerFrame);
            for (size_t offset = 0; offset < budget; ++offset) {
                const size_t fi = (cursor + offset) % total;
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
            (*ctx.layer_fallback_scan_cursor)[layer_idx] = (cursor + budget) % total;
            continue;
        }
        if (should_cluster_point_layer) {
            render_candidates.clear();
            render_candidates.reserve(l.features.size());
            for (size_t fi = 0; fi < l.features.size(); ++fi) render_candidates.push_back((uint32_t)fi);
            renderClusteredPointCandidates(
                ctx,
                layer_idx,
                l,
                render_candidates,
                base_color,
                is_heat_layer,
                is_zoning_layer,
                heat_normalization,
                normalization_group_key);
            continue;
        }
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
