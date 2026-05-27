#include "render_layer_pass.h"

#include "aggregate_debug.h"
#include "aggregate_visualization_strategies.h"
#include "app_utils.h"
#include "feature_props.h"
#include "geo.h"
#include "layer_geometry.h"
#include "map_render_hover.h"
#include "map_render_utils.h"
#include "render_routing.h"
#include "render_tile_cache.h"
#include "worldsim_app.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

namespace {

constexpr float kPointMarkerRadiusPx = 5.0f;
constexpr float kPointMarkerOutlinePx = 1.6f;
constexpr float kPointClusterCellPx = 28.0f;
constexpr float kPointClusterRadiusPx = 12.0f;
constexpr int kRenderTileSizePx = 256;

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
    size_t representative_feature_idx = (size_t)-1;
    const LayerDef::FeatureRecord* representative_feature = nullptr;
    float min_lon = 0.0f;
    float max_lon = 0.0f;
    float min_lat = 0.0f;
    float max_lat = 0.0f;
};

struct DeferredPointRenderJob {
    size_t layer_idx = 0;
    size_t feature_idx = (size_t)-1;
    const LayerDef* layer = nullptr;
    const LayerDef::FeatureRecord* feature = nullptr;
    ImU32 color = 0;
    uint64_t order_key = 0;
};

struct RuntimeRasterTileRequest {
    int tx = 0;
    int ty = 0;
    int wrapped_x = 0;
    std::filesystem::path path;
};

const PolygonGeometryArtifact* polygonArtifactForLayer(const RenderLayerPassContext& ctx, size_t layer_idx);

int wrapTileX(int x, int period) {
    if (period <= 0) return x;
    x %= period;
    if (x < 0) x += period;
    return x;
}

bool filterStateActive(const RenderLayerPassContext& ctx) {
    return ctx.filter_enabled ||
           (ctx.filter_blocklot && ctx.filter_blocklot[0] != '\0') ||
           (ctx.filter_status && ctx.filter_status[0] != '\0') ||
           (ctx.filter_address && ctx.filter_address[0] != '\0') ||
           (ctx.filter_owner && ctx.filter_owner[0] != '\0') ||
           (ctx.filter_zip && ctx.filter_zip[0] != '\0');
}

bool queryStateActive(const RenderLayerPassContext& ctx) {
    return ctx.query_layers && !ctx.query_layers->empty();
}

bool layerFillEnabledForRuntime(const RenderLayerPassContext& ctx, size_t layer_idx) {
    const bool layer_fill_enabled =
        ctx.layer_fill_enabled && layer_idx < ctx.layer_fill_enabled->size()
            ? (*ctx.layer_fill_enabled)[layer_idx]
            : true;
    const bool layer_polygon_fill_allowed =
        ctx.should_fill_layer_polygon ? ctx.should_fill_layer_polygon(layer_idx) : true;
    return layer_fill_enabled && layer_polygon_fill_allowed;
}

bool tryDrawPolygonRasterTileBase(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    const LayerDef& layer,
    LayerRenderRoute render_route,
    PolygonRasterTileMode raster_mode) {
    if (raster_mode == PolygonRasterTileMode::VectorOnly) return false;
    if (!ctx.root || !ctx.draw || !ctx.center_lon || !ctx.center_lat || ctx.zoom_scale <= 0.0f) return false;
    const PolygonGeometryArtifact* artifact = polygonArtifactForLayer(ctx, layer_idx);
    if (!artifact || artifact->header.source_signature.empty()) return false;

    const std::string style_key = renderPolygonFillStyleKey(layer, ctx.map_polygon_fill_opacity);
    const int period = 1 << ctx.math_zoom;
    const int max_tile = period - 1;
    const ImVec2 center_world = lonLatToWorldPx(static_cast<float>(*ctx.center_lon), static_cast<float>(*ctx.center_lat), ctx.math_zoom);
    const double half_w_world = (ctx.size.x * 0.5) / ctx.zoom_scale;
    const double half_h_world = (ctx.size.y * 0.5) / ctx.zoom_scale;
    const double epsilon = 1e-9;
    const int min_x = static_cast<int>(std::floor((center_world.x - half_w_world) / double(kRenderTileSizePx)));
    const int max_x = static_cast<int>(std::floor((center_world.x + half_w_world - epsilon) / double(kRenderTileSizePx)));
    const int min_y = static_cast<int>(std::floor((center_world.y - half_h_world) / double(kRenderTileSizePx)));
    const int max_y = static_cast<int>(std::floor((center_world.y + half_h_world - epsilon) / double(kRenderTileSizePx)));

    std::vector<RuntimeRasterTileRequest> requests;
    requests.reserve(std::max(0, (max_x - min_x + 1) * (max_y - min_y + 1)));
    for (int ty = min_y; ty <= max_y; ++ty) {
        if (ty < 0 || ty > max_tile) continue;
        for (int tx = min_x; tx <= max_x; ++tx) {
            const int wrapped_x = wrapTileX(tx, period);
            RenderTileCacheKey key;
            key.layer_file = layer.file;
            key.render_route = layerRenderRouteArtifactName(render_route);
            key.source_signature = artifact->header.source_signature;
            key.style_key = style_key;
            key.z = ctx.math_zoom;
            key.x = wrapped_x;
            key.y = ty;
            key.extension = "ppm";
            RuntimeRasterTileRequest request;
            request.tx = tx;
            request.ty = ty;
            request.wrapped_x = wrapped_x;
            request.path = renderTileCachePath(*ctx.root, key);
            requests.push_back(std::move(request));
        }
    }
    if (requests.empty()) return false;

    for (const RuntimeRasterTileRequest& request : requests) {
        std::error_code ec;
        if (!std::filesystem::exists(request.path, ec) || ec) return false;
    }

    for (const RuntimeRasterTileRequest& request : requests) {
        TileTexture* texture = getExactImageTexture(request.path);
        if (!texture || !texture->descriptor) return false;
    }

    for (const RuntimeRasterTileRequest& request : requests) {
        TileTexture* texture = getExactImageTexture(request.path);
        if (!texture || !texture->descriptor) return false;
        const ImVec2 tile_world(
            static_cast<float>(request.tx * kRenderTileSizePx),
            static_cast<float>(request.ty * kRenderTileSizePx));
        const ImVec2 p0 = ctx.project_world(tile_world);
        const ImVec2 p1(
            p0.x + static_cast<float>(kRenderTileSizePx * ctx.zoom_scale),
            p0.y + static_cast<float>(kRenderTileSizePx * ctx.zoom_scale));
        ctx.draw->AddImage(
            reinterpret_cast<ImTextureID>(texture->descriptor),
            p0,
            p1,
            ImVec2(0.0f, 0.0f),
            ImVec2(1.0f, 1.0f),
            IM_COL32(255, 255, 255, 255));
    }
    return true;
}

ImVec2 pointWorldPosition(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg) {
    if (ctx.point_geometry_artifacts) {
        auto it = ctx.point_geometry_artifacts->find(layer_idx);
        if (it != ctx.point_geometry_artifacts->end()) {
            const PointGeometryArtifact& artifact = it->second;
            if (feature_idx < artifact.positions.size()) {
                return lonLatToWorldPx(artifact.positions[feature_idx].x, artifact.positions[feature_idx].y, ctx.math_zoom);
            }
        }
    }
    return lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, ctx.math_zoom);
}

const PolylineGeometryArtifact* polylineArtifactForLayer(const RenderLayerPassContext& ctx, size_t layer_idx) {
    if (!ctx.polyline_geometry_artifacts) return nullptr;
    auto it = ctx.polyline_geometry_artifacts->find(layer_idx);
    if (it == ctx.polyline_geometry_artifacts->end()) return nullptr;
    return &it->second;
}

const PolygonGeometryArtifact* polygonArtifactForLayer(const RenderLayerPassContext& ctx, size_t layer_idx) {
    if (!ctx.polygon_geometry_artifacts) return nullptr;
    auto it = ctx.polygon_geometry_artifacts->find(layer_idx);
    if (it == ctx.polygon_geometry_artifacts->end()) return nullptr;
    return &it->second;
}

const ParcelRenderFeatureRecord* parcelRenderFeature(const RenderLayerPassContext& ctx, size_t parcel_idx) {
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

bool featureHasPolygonGeometry(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg) {
    if ((int)layer_idx == ctx.parcel_layer_idx) {
        return parcelRenderFeature(ctx, feature_idx) != nullptr;
    }
    if (const PolygonGeometryArtifact* artifact = polygonArtifactForLayer(ctx, layer_idx)) {
        if (feature_idx < artifact->features.size()) return true;
    }
    return !fg.rings.empty();
}

bool featureHasPointGeometry(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg) {
    if (ctx.point_geometry_artifacts) {
        auto it = ctx.point_geometry_artifacts->find(layer_idx);
        if (it != ctx.point_geometry_artifacts->end()) {
            return feature_idx < it->second.positions.size();
        }
    }
    return fg.rings.empty() && fg.paths.empty();
}

bool featureHasPolylineGeometry(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg) {
    if (const PolylineGeometryArtifact* artifact = polylineArtifactForLayer(ctx, layer_idx)) {
        if (feature_idx < artifact->features.size()) return true;
    }
    return !fg.paths.empty();
}

bool isHoveredPointFeature(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg) {
    if (!ctx.hover_state) return false;
    if (ctx.hover_state->hovered_point_layer_idx < 0) return false;
    if ((size_t)ctx.hover_state->hovered_point_layer_idx != layer_idx) return false;
    if (ctx.hover_state->hovered_point_idx != (size_t)-1) {
        return ctx.hover_state->hovered_point_idx == feature_idx;
    }
    return ctx.hover_state->hovered_point == &fg;
}

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
    const LayerDef::FeatureRecord& fg,
    ImU32 base_color,
    bool is_heat_layer,
    bool is_zoning_layer,
    const HeatNormalizationState& heat_normalization,
    const std::function<std::string(const LayerDef::FeatureRecord&)>& normalization_group_key,
    float& feature_heat_value,
    float& feature_normalized_value,
    bool& feature_heat_value_valid) {
    ImU32 feature_c = base_color;
    feature_heat_value = 0.0f;
    feature_normalized_value = 0.0f;
    feature_heat_value_valid = false;
    const bool use_gradient =
        ctx.layer_heatmap_use_gradient &&
        layer_idx < ctx.layer_heatmap_use_gradient->size()
            ? (*ctx.layer_heatmap_use_gradient)[layer_idx]
            : true;
    if (is_heat_layer && use_gradient) {
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

bool pointInWorldRings(const std::vector<std::vector<ImVec2>>& rings, float x, float y) {
    if (rings.empty() || !pointInRing(rings[0], x, y)) return false;
    for (size_t ri = 1; ri < rings.size(); ++ri) {
        if (pointInRing(rings[ri], x, y)) return false;
    }
    return true;
}

bool pointInTriangleWorld(const ImVec2& p, const ImVec2& a, const ImVec2& b, const ImVec2& c) {
    auto cross = [](const ImVec2& u, const ImVec2& v, const ImVec2& q) {
        return (v.x - u.x) * (q.y - u.y) - (v.y - u.y) * (q.x - u.x);
    };
    const float c1 = cross(a, b, p);
    const float c2 = cross(b, c, p);
    const float c3 = cross(c, a, p);
    const bool has_neg = (c1 < 0.0f) || (c2 < 0.0f) || (c3 < 0.0f);
    const bool has_pos = (c1 > 0.0f) || (c2 > 0.0f) || (c3 > 0.0f);
    return !(has_neg && has_pos);
}

bool pointInPolygonArtifactWorld(
    const RenderLayerPassContext& ctx,
    const PolygonGeometryArtifact& artifact,
    size_t feature_idx,
    float world_x,
    float world_y) {
    if (feature_idx >= artifact.features.size()) return false;
    const GeometryArtifactFeatureRecord& rec = artifact.features[feature_idx];
    const uint32_t index_end = rec.index_offset + rec.index_count;
    if (index_end > artifact.fill_indices.size()) return false;
    const ImVec2 p(world_x, world_y);
    for (uint32_t i = rec.index_offset; i + 2 < index_end; i += 3) {
        const uint32_t ia = artifact.fill_indices[i];
        const uint32_t ib = artifact.fill_indices[i + 1];
        const uint32_t ic = artifact.fill_indices[i + 2];
        if (ia >= artifact.vertices.size() || ib >= artifact.vertices.size() || ic >= artifact.vertices.size()) continue;
        const ImVec2 a = lonLatToWorldPx(artifact.vertices[ia].x, artifact.vertices[ia].y, ctx.math_zoom);
        const ImVec2 b = lonLatToWorldPx(artifact.vertices[ib].x, artifact.vertices[ib].y, ctx.math_zoom);
        const ImVec2 c = lonLatToWorldPx(artifact.vertices[ic].x, artifact.vertices[ic].y, ctx.math_zoom);
        if (pointInTriangleWorld(p, a, b, c)) return true;
    }
    return false;
}

char toLowerAsciiFast(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch + ('a' - 'A')) : ch;
}

bool containsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || needle.size() > haystack.size()) return false;
    const size_t limit = haystack.size() - needle.size();
    for (size_t i = 0; i <= limit; ++i) {
        size_t j = 0;
        for (; j < needle.size(); ++j) {
            if (toLowerAsciiFast(haystack[i + j]) != toLowerAsciiFast(needle[j])) break;
        }
        if (j == needle.size()) return true;
    }
    return false;
}

PointMarkerGlyph defaultPointMarkerGlyphForLayer(const LayerDef& layer) {
    switch (layer.category) {
        case LayerDef::Category::PublicHealth: return PointMarkerGlyph::Cross;
        case LayerDef::Category::Infrastructure: return PointMarkerGlyph::Square;
        case LayerDef::Category::Safety: return PointMarkerGlyph::Diamond;
        case LayerDef::Category::Zoning: return PointMarkerGlyph::Triangle;
        case LayerDef::Category::Housing:
        default: return PointMarkerGlyph::Circle;
    }
}

void ensureRenderClassificationCache(const LayerDef& layer) {
    if (layer.render_classification_cache_valid) return;

    layer.zoning_polygon_layer_cache =
        !layerUsesPointGeometry(layer) &&
        (layer.category == LayerDef::Category::Zoning ||
         containsCaseInsensitive(layer.file, "zoning") ||
         containsCaseInsensitive(layer.name, "zoning"));

    PointMarkerGlyph glyph = defaultPointMarkerGlyphForLayer(layer);
    if (containsCaseInsensitive(layer.name, "water")) glyph = PointMarkerGlyph::Droplet;
    else if (containsCaseInsensitive(layer.name, "health")) glyph = PointMarkerGlyph::Cross;
    else if (containsCaseInsensitive(layer.name, "school")) glyph = PointMarkerGlyph::Triangle;
    else if (containsCaseInsensitive(layer.name, "market")) glyph = PointMarkerGlyph::Diamond;
    else if (containsCaseInsensitive(layer.name, "police")) glyph = PointMarkerGlyph::Diamond;
    else if (containsCaseInsensitive(layer.name, "church")) glyph = PointMarkerGlyph::Plus;
    else if (containsCaseInsensitive(layer.name, "industry")) glyph = PointMarkerGlyph::Square;
    else if (containsCaseInsensitive(layer.name, "filling")) glyph = PointMarkerGlyph::Square;
    else if (containsCaseInsensitive(layer.name, "event") ||
             containsCaseInsensitive(layer.subcategory, "event") ||
             containsCaseInsensitive(layer.duckdb_role, "point_event")) {
        glyph = PointMarkerGlyph::Droplet;
    }

    layer.point_marker_glyph_cache = (uint8_t)glyph;
    layer.render_classification_cache_valid = true;
}

PointMarkerGlyph pointMarkerGlyphForLayerFeature(const LayerDef& layer, const LayerDef::FeatureRecord* fg = nullptr) {
    if (fg && isLikelyCrimePointLayer(layer)) {
        return static_cast<PointMarkerGlyph>(crimePointGlyphCode(*fg));
    }
    ensureRenderClassificationCache(layer);
    return (PointMarkerGlyph)layer.point_marker_glyph_cache;
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

bool validAggregateLonLat(float lon, float lat) {
    if (!std::isfinite(lon) || !std::isfinite(lat)) return false;
    if (lon < -180.0f || lon > 180.0f || lat < -90.0f || lat > 90.0f) return false;
    return !(std::fabs(lon) < 1.0e-5f && std::fabs(lat) < 1.0e-5f);
}

bool isZoningPolygonLayer(const LayerDef& layer) {
    ensureRenderClassificationCache(layer);
    return layer.zoning_polygon_layer_cache;
}

bool shouldClusterPointLayer(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    const LayerDef& layer,
    bool layer_uses_heatmap,
    bool layer_uses_lod_for_draw) {
    if (layer_uses_heatmap || layer_uses_lod_for_draw) return false;
    if (ctx.hover_state &&
        ctx.hover_state->hovered_point_layer_idx >= 0 &&
        (size_t)ctx.hover_state->hovered_point_layer_idx == layer_idx) {
        return false;
    }
    return ctx.heatmap_policy &&
           layerUsesPointGeometry(layer) &&
           layerUsesPointClustering(*ctx.heatmap_policy, layer_idx);
}

bool layerHasPrimaryGpuDraw(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    const LayerDef& layer,
    bool layer_uses_heatmap_for_cache,
    bool layer_uses_lod_for_draw) {
    const LayerRenderRoute render_route =
        classifyLayerRenderRoute(layer_idx, layer, ctx.parcel_layer_idx);
    if (render_route == LayerRenderRoute::ParcelGpu && parcelGpuDrawActive()) return true;
    if ((int)layer_idx == ctx.crime_nibrs_layer_idx &&
        shouldUseCrimePointPrimaryGpuDraw(
            crimePointGpuDrawActive(),
            layer_uses_heatmap_for_cache,
            layer_uses_lod_for_draw,
            ctx.heatmap_policy && layerUsesPointClustering(*ctx.heatmap_policy, layer_idx))) {
        return true;
    }
    if (render_route == LayerRenderRoute::PointGpu) {
        if (layer_uses_heatmap_for_cache || layer_uses_lod_for_draw) return false;
        if (ctx.heatmap_policy && layerUsesPointClustering(*ctx.heatmap_policy, layer_idx)) return false;
        return pointLayerGpuDrawActive(layer_idx);
    }
    if (render_route == LayerRenderRoute::PolylineGpu) return polylineLayerGpuDrawActive(layer_idx);
    const bool polygon_gpu_route =
        render_route == LayerRenderRoute::GenericPolygonGpu ||
        render_route == LayerRenderRoute::ParcelPolygonGpu;
    if (polygon_gpu_route &&
        (zoningGpuDrawActive(layer_idx) || zoningGpuOutlineDrawActive(layer_idx))) {
        return true;
    }
    return false;
}

void enqueuePrimaryGpuDrawForLayer(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    const LayerDef& layer,
    bool layer_uses_heatmap_for_cache,
    bool layer_uses_lod_for_draw,
    bool allow_polygon_fill,
    bool allow_polygon_outline) {
    const LayerRenderRoute render_route =
        classifyLayerRenderRoute(layer_idx, layer, ctx.parcel_layer_idx);
    if (render_route == LayerRenderRoute::ParcelGpu && parcelGpuDrawActive()) {
        enqueueParcelGpuDraw(ctx.draw);
        return;
    }
    if ((int)layer_idx == ctx.crime_nibrs_layer_idx &&
        shouldUseCrimePointPrimaryGpuDraw(
            crimePointGpuDrawActive(),
            layer_uses_heatmap_for_cache,
            layer_uses_lod_for_draw,
            ctx.heatmap_policy && layerUsesPointClustering(*ctx.heatmap_policy, layer_idx))) {
        enqueueCrimePointGpuDraw(ctx.draw);
        return;
    }
    if (render_route == LayerRenderRoute::PointGpu) {
        if (layer_uses_heatmap_for_cache || layer_uses_lod_for_draw) return;
        if (ctx.heatmap_policy && layerUsesPointClustering(*ctx.heatmap_policy, layer_idx)) return;
        if (pointLayerGpuDrawActive(layer_idx)) {
            enqueuePointLayerGpuDraw(ctx.draw, layer_idx);
        }
        return;
    }
    if (render_route == LayerRenderRoute::PolylineGpu) {
        if (polylineLayerGpuDrawActive(layer_idx)) {
            enqueuePolylineLayerGpuDraw(ctx.draw, layer_idx);
        }
        return;
    }
    const bool polygon_gpu_route =
        render_route == LayerRenderRoute::GenericPolygonGpu ||
        render_route == LayerRenderRoute::ParcelPolygonGpu;
    if (!polygon_gpu_route) return;
    if (allow_polygon_fill && zoningGpuDrawActive(layer_idx)) {
        enqueueZoningGpuDraw(ctx.draw, layer_idx);
    }
    if (allow_polygon_outline && zoningGpuOutlineDrawActive(layer_idx)) {
        enqueueZoningGpuOutlineDraw(ctx.draw, layer_idx);
    }
}

bool resolveFeatureRenderStyle(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef& layer,
    const LayerDef::FeatureRecord& fg,
    ImU32 base_color,
    bool is_heat_layer,
    bool is_zoning_layer,
    const HeatNormalizationState& heat_normalization,
    const std::function<std::string(const LayerDef::FeatureRecord&)>& normalization_group_key,
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
        featureHasPolygonGeometry(ctx, layer_idx, feature_idx, fg) ? 1 : 0);
    return true;
}

bool projectFeatureScreenBounds(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg,
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
    const std::function<std::string(const LayerDef::FeatureRecord&)>& normalization_group_key) {
    std::unordered_map<PointClusterCellKey, PointClusterBucket, PointClusterCellKeyHash> buckets;
    buckets.reserve(feature_indices.size());
    for (uint32_t fidx : feature_indices) {
        if ((size_t)fidx >= layer.features.size()) continue;
        const auto& fg = layer.features[(size_t)fidx];
        if (featureHasPolygonGeometry(ctx, layer_idx, (size_t)fidx, fg) ||
            featureHasPolylineGeometry(ctx, layer_idx, (size_t)fidx, fg)) {
            continue;
        }
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
        ImVec2 pw = pointWorldPosition(ctx, layer_idx, (size_t)fidx, fg);
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
            bucket.glyph = pointMarkerGlyphForLayerFeature(layer);
            bucket.representative_feature_idx = (size_t)fidx;
            bucket.representative_feature = &fg;
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
            const bool hovered =
                bucket.representative_feature &&
                bucket.representative_feature_idx != (size_t)-1 &&
                isHoveredPointFeature(
                    ctx,
                    layer_idx,
                    bucket.representative_feature_idx,
                    *bucket.representative_feature);
            drawPointMarker(
                ctx.draw,
                center,
                bucket.color,
                bucket.glyph,
                hovered ? (kPointMarkerRadiusPx + 1.8f) : kPointMarkerRadiusPx);
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
                    *ctx.zoom = std::min((double)ctx.max_zoom, std::max(*ctx.zoom + 2.0, (double)ctx.math_zoom + 1.0));
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
    const LayerDef::FeatureRecord& fg,
    const ImVec2& p0w,
    const ImVec2& p1w,
    ImU32 feature_c,
    float feature_sample_value,
    bool feature_heat_value_valid) {
    HeatSample base;
    if (!aggregateSampleAnchorLonLatForFeature(
            ctx,
            sample_layer_idx,
            feature_idx,
            fg,
            base.lon,
            base.lat)) {
        return;
    }
    base.color = ImGui::ColorConvertU32ToFloat4(feature_c);
    base.value = feature_sample_value;
    base.has_value = feature_heat_value_valid;
    base.prefer_gradient =
        sample_layer_idx < ctx.layer_heatmap_use_gradient->size()
            ? (*ctx.layer_heatmap_use_gradient)[sample_layer_idx]
            : true;
    resolveLayerHeatSettings(*ctx.heatmap_policy, sample_layer_idx, base);

    const int aggregate_algo = resolveLayerAggregateAlgo(*ctx.heatmap_policy, sample_layer_idx);
    const PolygonGeometryArtifact* polygon_artifact = polygonArtifactForLayer(ctx, sample_layer_idx);
    const bool artifact_ready = polygon_artifact && feature_idx < polygon_artifact->features.size();
    const bool area_choropleth =
        aggregate_algo == kAggregateMedianChoropleth &&
        feature_heat_value_valid &&
        artifact_ready;
    if (area_choropleth) {
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
                const bool inside =
                    pointInPolygonArtifactWorld(ctx, *polygon_artifact, feature_idx, cx, cy);
                if (!inside) continue;
                HeatSample hs = base;
                hs.x = cx;
                hs.y = cy;
                ctx.heat_samples->push_back(hs);
            }
        }
        if (ctx.heat_samples->size() > before) return;
    }

    HeatSample hs = base;
    if (featureHasPointGeometry(ctx, sample_layer_idx, feature_idx, fg)) {
        const ImVec2 anchor_world = lonLatToWorldPx(base.lon, base.lat, ctx.math_zoom);
        hs.x = anchor_world.x;
        hs.y = anchor_world.y;
    } else {
        hs.x = (p0w.x + p1w.x) * 0.5f;
        hs.y = (p0w.y + p1w.y) * 0.5f;
    }
    ctx.heat_samples->push_back(hs);
}

size_t addHeatSamplesForPointArtifact(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    const LayerDef& layer,
    ImU32 base_color,
    bool is_heat_layer,
    bool is_zoning_layer,
    const HeatNormalizationState& heat_normalization,
    const std::function<std::string(const LayerDef::FeatureRecord&)>& normalization_group_key) {
    if (!ctx.point_geometry_artifacts || !ctx.heat_samples || !ctx.heatmap_policy) return 0;
    auto artifact_it = ctx.point_geometry_artifacts->find(layer_idx);
    if (artifact_it == ctx.point_geometry_artifacts->end()) return 0;
    const PointGeometryArtifact& artifact = artifact_it->second;
    if (artifact.positions.empty()) return 0;

    const size_t before = ctx.heat_samples->size();
    for (size_t point_idx = 0; point_idx < artifact.positions.size(); ++point_idx) {
        const ImVec2 pos = artifact.positions[point_idx];
        if (!validAggregateLonLat(pos.x, pos.y)) continue;

        size_t feature_idx = point_idx;
        if (point_idx < artifact.feature_refs.size()) {
            const uint32_t feature_ref = artifact.feature_refs[point_idx];
            if (feature_ref < artifact.features.size()) {
                feature_idx = artifact.features[feature_ref].feature_idx;
            }
        }

        ImU32 feature_c = base_color;
        float feature_heat_value = 0.0f;
        float feature_normalized_value = 0.0f;
        bool feature_heat_value_valid = false;
        if (feature_idx < layer.features.size()) {
            const LayerDef::FeatureRecord& fg = layer.features[feature_idx];
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
                continue;
            }
        }

        HeatSample hs;
        hs.layer = static_cast<int>(layer_idx);
        hs.lon = pos.x;
        hs.lat = pos.y;
        const ImVec2 world = lonLatToWorldPx(pos.x, pos.y, ctx.math_zoom);
        hs.x = world.x;
        hs.y = world.y;
        hs.color = ImGui::ColorConvertU32ToFloat4(feature_c);
        hs.value = heat_normalization.normalize_mode == 0 ? feature_heat_value : feature_normalized_value;
        hs.has_value = feature_heat_value_valid;
        hs.prefer_gradient =
            ctx.layer_heatmap_use_gradient && layer_idx < ctx.layer_heatmap_use_gradient->size()
                ? (*ctx.layer_heatmap_use_gradient)[layer_idx]
                : true;
        resolveLayerHeatSettings(*ctx.heatmap_policy, layer_idx, hs);
        ctx.heat_samples->push_back(hs);
    }
    return ctx.heat_samples->size() - before;
}

void drawFeatureRecordetry(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef& layer,
    const LayerDef::FeatureRecord& fg,
    ImU32 feature_c,
    bool layer_uses_lod_for_draw,
    std::vector<DeferredPointRenderJob>* deferred_point_jobs) {
    const PolygonGeometryArtifact* polygon_artifact = polygonArtifactForLayer(ctx, layer_idx);
    const bool artifact_ready = polygon_artifact && feature_idx < polygon_artifact->features.size();
    const ParcelRenderFeatureRecord* parcel_feature =
        (int)layer_idx == ctx.parcel_layer_idx ? parcelRenderFeature(ctx, feature_idx) : nullptr;
    if (artifact_ready || parcel_feature || !fg.rings.empty()) {
        return;
    }

    if (featureHasPolylineGeometry(ctx, layer_idx, feature_idx, fg)) {
        return;
    }

    if (!featureHasPointGeometry(ctx, layer_idx, feature_idx, fg)) return;
    ImVec2 pw = pointWorldPosition(ctx, layer_idx, feature_idx, fg);
    ImVec2 ps = ctx.project_world(pw);
    if (ps.x >= ctx.origin.x && ps.x <= ctx.origin.x + ctx.size.x &&
        ps.y >= ctx.origin.y && ps.y <= ctx.origin.y + ctx.size.y) {
        if (deferred_point_jobs) {
            DeferredPointRenderJob job;
            job.layer_idx = layer_idx;
            job.feature_idx = feature_idx;
            job.layer = &layer;
            job.feature = &fg;
            job.color = feature_c;
            job.order_key = stablePointFeatureOrderKey(layer_idx, feature_idx, fg);
            deferred_point_jobs->push_back(job);
        } else {
            const bool hovered = isHoveredPointFeature(ctx, layer_idx, feature_idx, fg);
            drawPointMarker(
                ctx.draw,
                ps,
                feature_c,
                pointMarkerGlyphForLayerFeature(layer, &fg),
                hovered ? (kPointMarkerRadiusPx + 1.8f) : kPointMarkerRadiusPx);
            if (ctx.prof_features_drawn_frame) {
                ++(*ctx.prof_features_drawn_frame);
            }
        }
    }
}

void flushDeferredPointRenderJobs(
    const RenderLayerPassContext& ctx,
    const std::vector<DeferredPointRenderJob>& deferred_point_jobs) {
    for (const DeferredPointRenderJob& job : deferred_point_jobs) {
        if (!job.layer || !job.feature) continue;
        ImVec2 pw = pointWorldPosition(ctx, job.layer_idx, job.feature_idx, *job.feature);
        ImVec2 ps = ctx.project_world(pw);
        if (ps.x < ctx.origin.x || ps.x > ctx.origin.x + ctx.size.x ||
            ps.y < ctx.origin.y || ps.y > ctx.origin.y + ctx.size.y) {
            continue;
        }
        const bool hovered =
            job.feature &&
            isHoveredPointFeature(ctx, job.layer_idx, job.feature_idx, *job.feature);
        drawPointMarker(
            ctx.draw,
            ps,
            job.color,
            pointMarkerGlyphForLayerFeature(*job.layer, job.feature),
            hovered ? (kPointMarkerRadiusPx + 1.8f) : kPointMarkerRadiusPx);
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
    const LayerDef::FeatureRecord& fg,
    ImU32 base_color,
    bool is_heat_layer,
    bool is_zoning_layer,
    const HeatNormalizationState& heat_normalization,
    const std::function<std::string(const LayerDef::FeatureRecord&)>& normalization_group_key,
    bool layer_uses_heatmap,
    bool layer_uses_lod_for_draw,
    bool apply_smooth_stride,
    size_t smooth_sample_stride,
    std::vector<DeferredPointRenderJob>* deferred_point_jobs) {
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

    drawFeatureRecordetry(ctx, layer_idx, feature_idx, layer, fg, feature_c, layer_uses_lod_for_draw, deferred_point_jobs);
}

} // namespace

bool aggregateSampleAnchorLonLatForFeature(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg,
    float& out_lon,
    float& out_lat) {
    if (ctx.point_geometry_artifacts) {
        auto point_it = ctx.point_geometry_artifacts->find(layer_idx);
        if (point_it != ctx.point_geometry_artifacts->end() &&
            feature_idx < point_it->second.positions.size()) {
            const ImVec2 pos = point_it->second.positions[feature_idx];
            if (validAggregateLonLat(pos.x, pos.y)) {
                out_lon = pos.x;
                out_lat = pos.y;
                return true;
            }
        }
    }

    const float lon = (fg.extent.min_lon + fg.extent.max_lon) * 0.5f;
    const float lat = (fg.extent.min_lat + fg.extent.max_lat) * 0.5f;
    if (!validAggregateLonLat(lon, lat)) return false;
    out_lon = lon;
    out_lat = lat;
    return true;
}

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

bool shouldUseCrimePointPrimaryGpuDraw(
    bool crime_gpu_draw_active,
    bool layer_uses_heatmap_for_cache,
    bool layer_uses_lod_for_draw,
    bool layer_uses_point_clustering) {
    if (!crime_gpu_draw_active) return false;
    if (layer_uses_heatmap_for_cache) return false;
    if (layer_uses_lod_for_draw) return false;
    if (layer_uses_point_clustering) return false;
    return true;
}

void runRenderLayerPass(const RenderLayerPassContext& ctx) {
    std::vector<uint32_t> render_candidates;
    std::vector<DeferredPointRenderJob> deferred_point_jobs;
    for (size_t layer_idx : ctx.render_plan->draw_layer_order) {
        auto& l = (*ctx.layers)[layer_idx];
        if (!l.enabled) continue;
        if (ctx.layer_passes_filters && !ctx.layer_passes_filters(layer_idx)) continue;
        const bool layer_uses_heatmap_for_cache = layerUsesHeatmapAggregate(*ctx.heatmap_policy, layer_idx);
        const bool layer_uses_lod_for_draw = layerUsesLodGeometry(*ctx.heatmap_policy, layer_idx);
        const bool is_zoning_layer = isZoningPolygonLayer(l);
        const LayerRenderRoute render_route =
            classifyLayerRenderRoute(layer_idx, l, ctx.parcel_layer_idx);
        const PolygonRasterTilePolicyContext raster_policy_ctx{
            &l,
            render_route,
            ctx.math_zoom,
            layer_uses_heatmap_for_cache,
            layer_uses_lod_for_draw,
            layerFillEnabledForRuntime(ctx, layer_idx),
            filterStateActive(ctx),
            queryStateActive(ctx)
        };
        const PolygonRasterTileMode raster_mode = resolvePolygonRasterTileMode(raster_policy_ctx);
        const bool raster_tile_base_drawn =
            tryDrawPolygonRasterTileBase(ctx, layer_idx, l, render_route, raster_mode);
        const bool allow_polygon_fill = !raster_tile_base_drawn;
        const bool allow_polygon_outline =
            !raster_tile_base_drawn ||
            raster_mode == PolygonRasterTileMode::RasterBaseVectorOutline;
        enqueuePrimaryGpuDrawForLayer(
            ctx,
            layer_idx,
            l,
            layer_uses_heatmap_for_cache,
            layer_uses_lod_for_draw,
            allow_polygon_fill,
            allow_polygon_outline);
        if (raster_tile_base_drawn) {
            continue;
        }
        if (layerHasPrimaryGpuDraw(ctx, layer_idx, l, layer_uses_heatmap_for_cache, layer_uses_lod_for_draw)) {
            continue;
        }

        const bool stable_gpu_geometry_layer =
            layerUsesPolylineGeometry(l) ||
            (!layerUsesPointGeometry(l) && !layerUsesPolylineGeometry(l));
        if (stable_gpu_geometry_layer &&
            !layer_uses_heatmap_for_cache &&
            !layer_uses_lod_for_draw) {
            continue;
        }

        if (shouldSkipRawSourceLayer(layer_idx, ctx.raw_source_layer_policy) &&
            !layer_uses_heatmap_for_cache &&
            !layer_uses_lod_for_draw) {
            continue;
        }
        const bool is_heat_layer = !l.heatmap_field.empty();
        auto normalization_group_key = [&](const LayerDef::FeatureRecord& fg) {
            std::string key = firstDisplayProperty(fg, {
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
            shouldClusterPointLayer(ctx, layer_idx, l, layer_uses_heatmap_for_cache, layer_uses_lod_for_draw);
        if (ctx.should_recompute_heatmap &&
            layer_uses_heatmap_for_cache &&
            layerUsesPointGeometry(l)) {
            const size_t added = addHeatSamplesForPointArtifact(
                ctx,
                layer_idx,
                l,
                base_color,
                is_heat_layer,
                is_zoning_layer,
                heat_normalization,
                normalization_group_key);
            if (added > 0) {
                if (worldsimGpuAggregateDebugEnabled()) {
                    std::fprintf(
                        stderr,
                        "[worldsim3][gpu-aggregate] point-artifact-samples layer=%zu file=%s samples=%zu features=%zu\n",
                        layer_idx,
                        l.file.c_str(),
                        added,
                        l.features.size());
                }
                continue;
            }
        }
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
                if (!ctx.high_quality_gpu_aggregate &&
                    ctx.smooth_only_heatmap &&
                    ctx.should_recompute_heatmap &&
                    layer_uses_heatmap_for_cache &&
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
                    layer_uses_heatmap_for_cache,
                    layer_uses_lod_for_draw,
                    false,
                    1,
                    &deferred_point_jobs);
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
                layer_uses_heatmap_for_cache,
                layer_uses_lod_for_draw,
                true,
                smooth_sample_stride,
                &deferred_point_jobs);
        }
    }
    std::sort(
        deferred_point_jobs.begin(),
        deferred_point_jobs.end(),
        [&](const DeferredPointRenderJob& a, const DeferredPointRenderJob& b) {
            const int hovered_point_layer_idx =
                ctx.hover_state ? ctx.hover_state->hovered_point_layer_idx : -1;
            const bool a_hovered_layer =
                hovered_point_layer_idx >= 0 && a.layer_idx == (size_t)hovered_point_layer_idx;
            const bool b_hovered_layer =
                hovered_point_layer_idx >= 0 && b.layer_idx == (size_t)hovered_point_layer_idx;
            if (a_hovered_layer != b_hovered_layer) return !a_hovered_layer && b_hovered_layer;
            return a.order_key < b.order_key;
        });
    flushDeferredPointRenderJobs(ctx, deferred_point_jobs);
}
