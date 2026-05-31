#include "map_tab.h"

#include "app_utils.h"
#include "choropleth_histogram.h"
#include "feature_props.h"
#include "geo.h"
#include "map_render_utils.h"
#include "map_overlay_panels.h"
#include "map_title_source.h"
#include "owner_info.h"
#include "road_label.h"
#include "cache_io.h"
#include "render_routing.h"
#include "ui_fonts.h"
#include "worldsim_app.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
std::string trimCopy(const std::string& value);

struct MapCornerControlState {
    bool hovered = false;
    bool fullscreen_hovered = false;
    bool snapshot_hovered = false;
    bool video_hovered = false;
};

struct RoadLabelPropertyCache {
    std::string signature;
    bool attempted = false;
    bool loaded = false;
    std::vector<LayerDef::FeatureProperties> properties;
};

struct RoadLabelArtifactCache {
    std::string signature;
    bool attempted = false;
    bool loaded = false;
    PolylineGeometryArtifact artifact;
};

std::unordered_map<size_t, RoadLabelPropertyCache> g_road_label_property_cache;
std::unordered_map<size_t, RoadLabelArtifactCache> g_road_label_artifact_cache;

void mapCornerControlPositions(
    const MapCanvasSession& session,
    ImVec2& fullscreen_min,
    ImVec2& camera_min,
    ImVec2& video_min) {
    constexpr float button = 34.0f;
    constexpr float gap = 8.0f;
    video_min = ImVec2(
        session.origin.x + session.size.x - 12.0f - button,
        session.origin.y + session.size.y - 12.0f - button);
    camera_min = ImVec2(video_min.x - gap - button, video_min.y);
    fullscreen_min = ImVec2(camera_min.x - gap - button, camera_min.y);
}

void drawMapIconFrame(
    ImDrawList* draw,
    const ImVec2& min,
    const ImVec2& size,
    bool hovered) {
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const ImU32 fill = hovered ? IM_COL32(32, 48, 60, 235) : IM_COL32(17, 24, 32, 215);
    draw->AddRectFilled(min, max, fill, 8.0f);
    draw->AddRect(min, max, hovered ? IM_COL32(125, 220, 255, 180) : IM_COL32(255, 255, 255, 80), 8.0f);
}

void drawMapFullscreenIcon(ImDrawList* draw, const ImVec2& min, bool hovered) {
    constexpr float button = 34.0f;
    drawMapIconFrame(draw, min, ImVec2(button, button), hovered);
    const ImU32 c = IM_COL32(245, 248, 250, 240);
    const float x0 = min.x + 10.0f;
    const float y0 = min.y + 10.0f;
    const float x1 = min.x + button - 10.0f;
    const float y1 = min.y + button - 10.0f;
    const float arm = 6.0f;
    draw->AddLine(ImVec2(x0, y0), ImVec2(x0 + arm, y0), c, 1.8f);
    draw->AddLine(ImVec2(x0, y0), ImVec2(x0, y0 + arm), c, 1.8f);
    draw->AddLine(ImVec2(x1, y0), ImVec2(x1 - arm, y0), c, 1.8f);
    draw->AddLine(ImVec2(x1, y0), ImVec2(x1, y0 + arm), c, 1.8f);
    draw->AddLine(ImVec2(x0, y1), ImVec2(x0 + arm, y1), c, 1.8f);
    draw->AddLine(ImVec2(x0, y1), ImVec2(x0, y1 - arm), c, 1.8f);
    draw->AddLine(ImVec2(x1, y1), ImVec2(x1 - arm, y1), c, 1.8f);
    draw->AddLine(ImVec2(x1, y1), ImVec2(x1, y1 - arm), c, 1.8f);
}

void drawMapCameraIcon(ImDrawList* draw, const ImVec2& min, bool hovered) {
    constexpr float button = 34.0f;
    drawMapIconFrame(draw, min, ImVec2(button, button), hovered);
    const ImU32 c = IM_COL32(245, 248, 250, 240);
    const ImVec2 body_min(min.x + 8.0f, min.y + 12.0f);
    const ImVec2 body_max(min.x + button - 7.0f, min.y + button - 9.0f);
    draw->AddRect(body_min, body_max, c, 3.0f, 0, 1.8f);
    draw->AddRectFilled(ImVec2(min.x + 12.0f, min.y + 9.0f), ImVec2(min.x + 21.0f, min.y + 13.0f), c, 2.0f);
    draw->AddCircle(ImVec2(min.x + 17.5f, min.y + 20.5f), 4.6f, c, 18, 1.8f);
    draw->AddCircleFilled(ImVec2(min.x + 26.0f, min.y + 15.0f), 1.4f, c, 8);
}

void drawMapVideoIcon(ImDrawList* draw, const ImVec2& min, bool hovered, bool recording) {
    constexpr float button = 34.0f;
    drawMapIconFrame(draw, min, ImVec2(button, button), hovered || recording);
    const ImU32 c = recording ? IM_COL32(255, 92, 92, 245) : IM_COL32(245, 248, 250, 240);
    const ImVec2 body_min(min.x + 8.0f, min.y + 11.0f);
    const ImVec2 body_max(min.x + 21.0f, min.y + 23.0f);
    draw->AddRect(body_min, body_max, c, 3.0f, 0, 1.8f);
    draw->AddTriangle(
        ImVec2(min.x + 22.0f, min.y + 16.0f),
        ImVec2(min.x + 28.0f, min.y + 12.0f),
        ImVec2(min.x + 28.0f, min.y + 22.0f),
        c,
        1.8f);
    if (recording) draw->AddCircleFilled(ImVec2(min.x + 14.5f, min.y + 17.0f), 3.2f, c, 16);
}

void drawMapFpsOverlay(const MapCanvasSession& session) {
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (!draw) return;
    const float fps = ImGui::GetIO().Framerate;
    char label[32];
    std::snprintf(label, sizeof(label), "FPS %.1f", fps);

    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 pad(10.0f, 6.0f);
    const float box_w = text_size.x + pad.x * 2.0f;
    const float box_h = text_size.y + pad.y * 2.0f;
    const ImVec2 min(
        session.origin.x + 12.0f,
        session.origin.y + std::max(12.0f, session.size.y - box_h - 12.0f));
    const ImVec2 max(min.x + box_w, min.y + box_h);
    draw->PushClipRect(session.origin, ImVec2(session.origin.x + session.size.x, session.origin.y + session.size.y), true);
    draw->AddRectFilled(min, max, IM_COL32(17, 24, 32, 215), 8.0f);
    draw->AddRect(min, max, IM_COL32(255, 255, 255, 80), 8.0f);
    draw->AddText(ImVec2(min.x + pad.x, min.y + pad.y), IM_COL32(245, 248, 250, 240), label);
    draw->PopClipRect();
}

bool featureExtentNearScreenPoint(
    const MapCanvasSession& session,
    const LayerDef::FeatureRecord& fg,
    const ImVec2& mouse,
    float tolerance_px) {
    const ImVec2 nw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.max_lat, session.math_zoom);
    const ImVec2 se = lonLatToWorldPx(fg.extent.max_lon, fg.extent.min_lat, session.math_zoom);
    const ImVec2 a = session.project_world(nw);
    const ImVec2 b = session.project_world(se);
    const float min_x = std::min(a.x, b.x) - tolerance_px;
    const float max_x = std::max(a.x, b.x) + tolerance_px;
    const float min_y = std::min(a.y, b.y) - tolerance_px;
    const float max_y = std::max(a.y, b.y) + tolerance_px;
    return mouse.x >= min_x && mouse.x <= max_x && mouse.y >= min_y && mouse.y <= max_y;
}

float normalizeRoadLabelAngle(float angle_rad) {
    constexpr float pi = 3.14159265358979323846f;
    while (angle_rad <= -pi) angle_rad += 2.0f * pi;
    while (angle_rad > pi) angle_rad -= 2.0f * pi;
    if (angle_rad > pi * 0.5f) angle_rad -= pi;
    if (angle_rad < -pi * 0.5f) angle_rad += pi;
    return angle_rad;
}

ImVec2 rotatePointAround(const ImVec2& point, const ImVec2& center, float angle_rad) {
    const float s = std::sin(angle_rad);
    const float c = std::cos(angle_rad);
    const float x = point.x - center.x;
    const float y = point.y - center.y;
    return ImVec2(center.x + x * c - y * s, center.y + x * s + y * c);
}

void rotateDrawListVertices(ImDrawList* draw, int vtx_start, const ImVec2& center, float angle_rad) {
    if (!draw || std::abs(angle_rad) < 0.001f) return;
    for (int i = vtx_start; i < draw->VtxBuffer.Size; ++i) {
        draw->VtxBuffer[i].pos = rotatePointAround(draw->VtxBuffer[i].pos, center, angle_rad);
    }
}

bool tryPickRoadLabelFromPolylineArtifact(
    const MapTabContext& ctx,
    const MapCanvasSession& session,
    size_t layer_idx,
    const PolylineGeometryArtifact& artifact,
    float tolerance_px,
    int& best_layer_idx,
    size_t& best_feature_idx,
    ImVec2& best_anchor,
    ImVec2& best_anchor_lonlat,
    float& best_angle_rad,
    float& best_dist_sq) {
    const LayerDef& layer = (*ctx.layers)[layer_idx];
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float tolerance_sq = tolerance_px * tolerance_px;
    bool found = false;
    for (const GeometryArtifactChunkRecord& chunk : artifact.chunks) {
        LayerDef::FeatureRecord chunk_extent{};
        chunk_extent.extent.min_lon = chunk.min_lon;
        chunk_extent.extent.min_lat = chunk.min_lat;
        chunk_extent.extent.max_lon = chunk.max_lon;
        chunk_extent.extent.max_lat = chunk.max_lat;
        if (!featureExtentNearScreenPoint(session, chunk_extent, mouse, tolerance_px)) continue;
        const uint32_t index_begin = chunk.index_offset;
        const uint32_t index_end = chunk.index_offset + chunk.index_count;
        for (uint32_t i = index_begin; i + 1 < index_end && i + 1 < artifact.line_indices.size(); i += 2) {
            const uint32_t ia = artifact.line_indices[i];
            const uint32_t ib = artifact.line_indices[i + 1];
            if (ia >= artifact.vertices.size() || ib >= artifact.vertices.size() || ia >= artifact.feature_refs.size()) continue;
            const uint32_t artifact_feature_ref = artifact.feature_refs[ia];
            if (artifact_feature_ref >= artifact.features.size()) continue;
            const size_t feature_idx = artifact.features[artifact_feature_ref].feature_idx;
            const ImVec2 aw = lonLatToWorldPx(artifact.vertices[ia].x, artifact.vertices[ia].y, session.math_zoom);
            const ImVec2 bw = lonLatToWorldPx(artifact.vertices[ib].x, artifact.vertices[ib].y, session.math_zoom);
            const ImVec2 a = session.project_world(aw);
            const ImVec2 b = session.project_world(bw);
            ImVec2 closest;
            const float dist_sq = squaredDistancePointToSegment(mouse, a, b, &closest);
            if (dist_sq <= tolerance_sq && dist_sq < best_dist_sq) {
                const float vx = b.x - a.x;
                const float vy = b.y - a.y;
                const float len_sq = vx * vx + vy * vy;
                const float t = len_sq > 0.0f
                    ? std::clamp(((mouse.x - a.x) * vx + (mouse.y - a.y) * vy) / len_sq, 0.0f, 1.0f)
                    : 0.0f;
                best_dist_sq = dist_sq;
                best_layer_idx = (int)layer_idx;
                best_feature_idx = feature_idx;
                best_anchor = closest;
                best_anchor_lonlat = ImVec2(
                    artifact.vertices[ia].x + (artifact.vertices[ib].x - artifact.vertices[ia].x) * t,
                    artifact.vertices[ia].y + (artifact.vertices[ib].y - artifact.vertices[ia].y) * t);
                best_angle_rad = normalizeRoadLabelAngle(std::atan2(b.y - a.y, b.x - a.x));
                found = true;
            }
        }
    }
    return found;
}

bool tryPickRoadLabelFromArtifact(
    const MapTabContext& ctx,
    const MapCanvasSession& session,
    size_t layer_idx,
    float tolerance_px,
    int& best_layer_idx,
    size_t& best_feature_idx,
    ImVec2& best_anchor,
    ImVec2& best_anchor_lonlat,
    float& best_angle_rad,
    float& best_dist_sq) {
    if (!ctx.polyline_geometry_artifacts) return false;
    auto artifact_it = ctx.polyline_geometry_artifacts->find(layer_idx);
    if (artifact_it == ctx.polyline_geometry_artifacts->end()) return false;
    return tryPickRoadLabelFromPolylineArtifact(
        ctx,
        session,
        layer_idx,
        artifact_it->second,
        tolerance_px,
        best_layer_idx,
        best_feature_idx,
        best_anchor,
        best_anchor_lonlat,
        best_angle_rad,
        best_dist_sq);
}

const PolylineGeometryArtifact* selectableRoadLabelArtifactForLayer(
    const MapTabContext& ctx,
    size_t layer_idx) {
    if (!ctx.root || !ctx.layers || layer_idx >= ctx.layers->size()) return nullptr;
    const LayerDef& layer = (*ctx.layers)[layer_idx];
    const std::filesystem::path layer_path = resolveStoredLayerPath(*ctx.root, layer);
    std::string sig;
    if (!resolveLayerSourceSignature(layer_path, sig, nullptr) || sig.empty()) return nullptr;

    RoadLabelArtifactCache& cache = g_road_label_artifact_cache[layer_idx];
    if (cache.signature != sig) {
        cache = RoadLabelArtifactCache{};
        cache.signature = sig;
    }
    if (!cache.attempted) {
        cache.attempted = true;
        const std::filesystem::path artifact_path =
            geometryArtifactCachePathForLayerFile(
                *ctx.root,
                layer.file,
                GeometryArtifactClass::Polyline,
                layerRenderRouteArtifactName(LayerRenderRoute::PolylineGpu));
        cache.loaded = loadBinaryPolylineGeometryArtifact(artifact_path, sig, cache.artifact);
    }
    return cache.loaded ? &cache.artifact : nullptr;
}

bool tryPickRoadLabelFromLayerFeatures(
    const MapTabContext& ctx,
    const MapCanvasSession& session,
    size_t layer_idx,
    float tolerance_px,
    int& best_layer_idx,
    size_t& best_feature_idx,
    ImVec2& best_anchor,
    ImVec2& best_anchor_lonlat,
    float& best_angle_rad,
    float& best_dist_sq) {
    const LayerDef& layer = (*ctx.layers)[layer_idx];
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float tolerance_sq = tolerance_px * tolerance_px;
    bool found = false;
    for (size_t feature_idx = 0; feature_idx < layer.features.size(); ++feature_idx) {
        const LayerDef::FeatureRecord& fg = layer.features[feature_idx];
        if (fg.paths.empty() || !featureExtentNearScreenPoint(session, fg, mouse, tolerance_px)) continue;
        for (const std::vector<ImVec2>& path : fg.paths) {
            if (path.size() < 2) continue;
            for (size_t i = 1; i < path.size(); ++i) {
                const ImVec2 aw = lonLatToWorldPx(path[i - 1].x, path[i - 1].y, session.math_zoom);
                const ImVec2 bw = lonLatToWorldPx(path[i].x, path[i].y, session.math_zoom);
                const ImVec2 a = session.project_world(aw);
                const ImVec2 b = session.project_world(bw);
                ImVec2 closest;
                const float dist_sq = squaredDistancePointToSegment(mouse, a, b, &closest);
                if (dist_sq <= tolerance_sq && dist_sq < best_dist_sq) {
                    const float vx = b.x - a.x;
                    const float vy = b.y - a.y;
                    const float len_sq = vx * vx + vy * vy;
                    const float t = len_sq > 0.0f
                        ? std::clamp(((mouse.x - a.x) * vx + (mouse.y - a.y) * vy) / len_sq, 0.0f, 1.0f)
                        : 0.0f;
                    best_dist_sq = dist_sq;
                    best_layer_idx = (int)layer_idx;
                    best_feature_idx = feature_idx;
                    best_anchor = closest;
                    best_anchor_lonlat = ImVec2(
                        path[i - 1].x + (path[i].x - path[i - 1].x) * t,
                        path[i - 1].y + (path[i].y - path[i - 1].y) * t);
                    best_angle_rad = normalizeRoadLabelAngle(std::atan2(b.y - a.y, b.x - a.x));
                    found = true;
                }
            }
        }
    }
    return found;
}

std::string resolveRoadLabelForPickedFeature(const MapTabContext& ctx, size_t layer_idx, size_t feature_idx) {
    const LayerDef& layer = (*ctx.layers)[layer_idx];
    if (feature_idx < layer.feature_properties.size() || feature_idx < layer.features.size()) {
        return roadLabelForFeature(layer, feature_idx);
    }
    if (!ctx.root) return "Unnamed road";

    const std::filesystem::path layer_path = resolveStoredLayerPath(*ctx.root, layer);
    std::string sig;
    if (!resolveLayerSourceSignature(layer_path, sig, nullptr) || sig.empty()) {
        return "Unnamed road";
    }

    RoadLabelPropertyCache& cache = g_road_label_property_cache[layer_idx];
    if (cache.signature != sig) {
        cache = RoadLabelPropertyCache{};
        cache.signature = sig;
    }
    if (!cache.attempted) {
        cache.attempted = true;
        std::vector<LayerDef::FeatureRecord> ignored_features;
        cache.loaded = loadCanonicalLayerFeatureCollection(
            *ctx.root,
            layer.file,
            sig,
            ignored_features,
            &cache.properties);
    }
    if (cache.loaded && feature_idx < cache.properties.size()) {
        return roadLabelForFeatureProperties(cache.properties[feature_idx]);
    }
    return "Unnamed road";
}

bool roadLabelScreenBox(
    const RoadLabelSelection& state,
    const MapCanvasSession& session,
    float size_scale,
    ImVec2& anchor,
    ImVec2& min,
    ImVec2& max) {
    const ImVec2 map_min = session.origin;
    const ImVec2 map_max(session.origin.x + session.size.x, session.origin.y + session.size.y);
    const ImVec2 anchor_world = lonLatToWorldPx(state.anchor_lonlat.x, state.anchor_lonlat.y, session.math_zoom);
    anchor = session.project_world(anchor_world);
    if (anchor.x < map_min.x || anchor.x > map_max.x || anchor.y < map_min.y || anchor.y > map_max.y) {
        return false;
    }

    const std::string caption = state.label;
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize() * std::clamp(size_scale, 0.65f, 2.5f);
    const ImVec2 text_size = font
        ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, caption.c_str())
        : ImGui::CalcTextSize(caption.c_str());
    const ImVec2 pad(10.0f * std::clamp(size_scale, 0.65f, 2.5f), 6.0f * std::clamp(size_scale, 0.65f, 2.5f));
    const float box_w = text_size.x + pad.x * 2.0f;
    const float box_h = text_size.y + pad.y * 2.0f;
    min = ImVec2(anchor.x - box_w * 0.5f, anchor.y - box_h * 0.5f);
    max = ImVec2(min.x + box_w, min.y + box_h);
    if (min.x < map_min.x + 8.0f) {
        min.x = map_min.x + 8.0f;
        max.x = min.x + box_w;
    }
    if (max.x > map_max.x - 8.0f) {
        min.x = std::max(map_min.x + 8.0f, map_max.x - 8.0f - box_w);
        max.x = min.x + box_w;
    }
    if (min.y < map_min.y + 8.0f) {
        min.y = anchor.y + 14.0f;
        max.y = min.y + box_h;
    }
    return true;
}

struct RoadLabelLayoutItem {
    const RoadLabelSelection* state = nullptr;
    std::string caption;
    ImVec2 anchor = ImVec2(0.0f, 0.0f);
    ImVec2 center = ImVec2(0.0f, 0.0f);
    ImVec2 half_size = ImVec2(0.0f, 0.0f);
    ImVec2 collision_half_size = ImVec2(0.0f, 0.0f);
    ImVec2 min = ImVec2(0.0f, 0.0f);
    ImVec2 max = ImVec2(0.0f, 0.0f);
    ImVec2 collision_min = ImVec2(0.0f, 0.0f);
    ImVec2 collision_max = ImVec2(0.0f, 0.0f);
    float angle_rad = 0.0f;
};

void updateRoadLabelLayoutBox(RoadLabelLayoutItem& item) {
    item.min = ImVec2(item.center.x - item.half_size.x, item.center.y - item.half_size.y);
    item.max = ImVec2(item.center.x + item.half_size.x, item.center.y + item.half_size.y);
    const float s = std::abs(std::sin(item.angle_rad));
    const float c = std::abs(std::cos(item.angle_rad));
    item.collision_half_size = ImVec2(
        c * item.half_size.x + s * item.half_size.y,
        s * item.half_size.x + c * item.half_size.y);
    item.collision_min = ImVec2(item.center.x - item.collision_half_size.x, item.center.y - item.collision_half_size.y);
    item.collision_max = ImVec2(item.center.x + item.collision_half_size.x, item.center.y + item.collision_half_size.y);
}

void clampRoadLabelLayoutBox(RoadLabelLayoutItem& item, const ImVec2& map_min, const ImVec2& map_max) {
    item.center.x = std::clamp(item.center.x, map_min.x + 8.0f + item.collision_half_size.x, map_max.x - 8.0f - item.collision_half_size.x);
    item.center.y = std::clamp(item.center.y, map_min.y + 8.0f + item.collision_half_size.y, map_max.y - 8.0f - item.collision_half_size.y);
    updateRoadLabelLayoutBox(item);
}

float roadLabelOverlapArea(const ImVec2& a_min, const ImVec2& a_max, const ImVec2& b_min, const ImVec2& b_max) {
    const float overlap_x = std::min(a_max.x, b_max.x) - std::max(a_min.x, b_min.x);
    const float overlap_y = std::min(a_max.y, b_max.y) - std::max(a_min.y, b_min.y);
    return overlap_x > 0.0f && overlap_y > 0.0f ? overlap_x * overlap_y : 0.0f;
}

ImVec2 roadLabelTitleReserveMin(const MapCanvasSession& session, const AppSettings* app_settings) {
    if (!app_settings || trimCopy(app_settings->map_title_text).empty()) return ImVec2(0.0f, 0.0f);
    return ImVec2(session.origin.x + 40.0f, session.origin.y + 8.0f);
}

ImVec2 roadLabelTitleReserveMax(const MapCanvasSession& session, const AppSettings* app_settings) {
    if (!app_settings || trimCopy(app_settings->map_title_text).empty()) return ImVec2(0.0f, 0.0f);
    return ImVec2(session.origin.x + session.size.x - 40.0f, session.origin.y + 132.0f);
}

int roadLabelCollisionMode(const AppSettings* app_settings) {
    if (!app_settings || !app_settings->road_label_avoid_overlap) return 0;
    return std::clamp(app_settings->road_label_collision_mode, 1, 3);
}

float roadLabelEffectiveSeparation(const AppSettings* app_settings) {
    if (!app_settings) return 8.0f;
    const float base = std::clamp(app_settings->road_label_separation_px, 0.0f, 48.0f);
    switch (roadLabelCollisionMode(app_settings)) {
        case 1: return base * 0.35f;
        case 3: return base * 1.6f;
        case 2:
        default: return base;
    }
}

void applyRoadLabelCandidateLayout(
    std::vector<RoadLabelLayoutItem>& items,
    const MapCanvasSession& session,
    const AppSettings* app_settings) {
    if (items.empty()) return;
    const int mode = roadLabelCollisionMode(app_settings);
    if (mode == 0) return;
    const ImVec2 map_min = session.origin;
    const ImVec2 map_max(session.origin.x + session.size.x, session.origin.y + session.size.y);
    const bool has_title_reserve = app_settings && !trimCopy(app_settings->map_title_text).empty();
    const ImVec2 title_min = roadLabelTitleReserveMin(session, app_settings);
    const ImVec2 title_max = roadLabelTitleReserveMax(session, app_settings);
    std::vector<RoadLabelLayoutItem> placed;
    placed.reserve(items.size());
    const float separation = roadLabelEffectiveSeparation(app_settings);
    const float movement_weight = mode == 1 ? 0.055f : (mode == 3 ? 0.006f : 0.015f);
    const float title_weight = mode == 1 ? 4.0f : (mode == 3 ? 20.0f : 12.0f);
    const float collision_weight = mode == 1 ? 7.0f : (mode == 3 ? 28.0f : 18.0f);
    const float offset_scale = mode == 1 ? 0.55f : (mode == 3 ? 1.35f : 1.0f);

    for (RoadLabelLayoutItem& item : items) {
        const float t_x = std::cos(item.angle_rad);
        const float t_y = std::sin(item.angle_rad);
        const ImVec2 tangent(t_x, t_y);
        const ImVec2 normal(-t_y, t_x);
        const float near_step = std::max(12.0f, item.collision_half_size.y + separation) * offset_scale;
        const float far_step = near_step + std::max(18.0f, item.collision_half_size.y) * offset_scale;
        const float along_step = std::max(16.0f, item.collision_half_size.x * 0.33f) * offset_scale;
        const ImVec2 offsets[] = {
            ImVec2(0.0f, 0.0f),
            ImVec2(normal.x * near_step, normal.y * near_step),
            ImVec2(-normal.x * near_step, -normal.y * near_step),
            ImVec2(normal.x * far_step, normal.y * far_step),
            ImVec2(-normal.x * far_step, -normal.y * far_step),
            ImVec2(tangent.x * along_step, tangent.y * along_step),
            ImVec2(-tangent.x * along_step, -tangent.y * along_step),
            ImVec2(tangent.x * along_step + normal.x * near_step, tangent.y * along_step + normal.y * near_step),
            ImVec2(-tangent.x * along_step - normal.x * near_step, -tangent.y * along_step - normal.y * near_step)
        };

        RoadLabelLayoutItem best = item;
        float best_score = std::numeric_limits<float>::max();
        for (const ImVec2& offset : offsets) {
            RoadLabelLayoutItem candidate = item;
            candidate.center = ImVec2(item.anchor.x + offset.x, item.anchor.y + offset.y);
            updateRoadLabelLayoutBox(candidate);
            clampRoadLabelLayoutBox(candidate, map_min, map_max);

            float score = (candidate.center.x - item.anchor.x) * (candidate.center.x - item.anchor.x) * movement_weight +
                          (candidate.center.y - item.anchor.y) * (candidate.center.y - item.anchor.y) * movement_weight;
            if (has_title_reserve) {
                score += roadLabelOverlapArea(candidate.collision_min, candidate.collision_max, title_min, title_max) * title_weight;
            }
            for (const RoadLabelLayoutItem& placed_item : placed) {
                const ImVec2 expanded_min(placed_item.collision_min.x - separation, placed_item.collision_min.y - separation);
                const ImVec2 expanded_max(placed_item.collision_max.x + separation, placed_item.collision_max.y + separation);
                score += roadLabelOverlapArea(candidate.collision_min, candidate.collision_max, expanded_min, expanded_max) * collision_weight;
            }
            const float edge_penalty =
                std::max(0.0f, 20.0f - (candidate.collision_min.x - map_min.x)) +
                std::max(0.0f, 20.0f - (candidate.collision_min.y - map_min.y)) +
                std::max(0.0f, 20.0f - (map_max.x - candidate.collision_max.x)) +
                std::max(0.0f, 20.0f - (map_max.y - candidate.collision_max.y));
            score += edge_penalty * 50.0f;
            if (score < best_score) {
                best_score = score;
                best = candidate;
            }
        }
        item = best;
        placed.push_back(item);
    }
}

void applyRoadLabelOverlapLayout(std::vector<RoadLabelLayoutItem>& items, const ImVec2& map_min, const ImVec2& map_max, const AppSettings* app_settings) {
    if (items.size() < 2) return;
    const int mode = roadLabelCollisionMode(app_settings);
    if (mode == 0) return;
    const float separation = roadLabelEffectiveSeparation(app_settings);
    const int iterations = mode == 1 ? 7 : (mode == 3 ? 28 : 18);
    const float tether = mode == 1 ? 0.20f : (mode == 3 ? 0.035f : 0.075f);
    const float max_step = mode == 1 ? 7.0f : (mode == 3 ? 24.0f : 14.0f);
    for (int iter = 0; iter < iterations; ++iter) {
        for (size_t i = 0; i < items.size(); ++i) {
            for (size_t j = i + 1; j < items.size(); ++j) {
                RoadLabelLayoutItem& a = items[i];
                RoadLabelLayoutItem& b = items[j];
                const float dx = b.center.x - a.center.x;
                const float dy = b.center.y - a.center.y;
                const float overlap_x = a.collision_half_size.x + b.collision_half_size.x + separation - std::abs(dx);
                const float overlap_y = a.collision_half_size.y + b.collision_half_size.y + separation - std::abs(dy);
                if (overlap_x <= 0.0f || overlap_y <= 0.0f) continue;

                if (overlap_x < overlap_y) {
                    const float dir = dx < 0.0f ? -1.0f : 1.0f;
                    const float step = std::min(overlap_x * 0.5f, max_step);
                    a.center.x -= dir * step;
                    b.center.x += dir * step;
                } else {
                    const float dir = dy < 0.0f ? -1.0f : 1.0f;
                    const float step = std::min(overlap_y * 0.5f, max_step);
                    a.center.y -= dir * step;
                    b.center.y += dir * step;
                }
            }
        }
        for (RoadLabelLayoutItem& item : items) {
            item.center.x += (item.anchor.x - item.center.x) * tether;
            item.center.y += (item.anchor.y - item.center.y) * tether;
            clampRoadLabelLayoutBox(item, map_min, map_max);
        }
    }
}

std::vector<RoadLabelLayoutItem> buildRoadLabelLayout(
    const RoadLabelState* label_state,
    const MapCanvasSession& session,
    const AppSettings* app_settings) {
    std::vector<RoadLabelLayoutItem> items;
    if (!label_state || !label_state->visible || label_state->selections.empty()) return items;
    const float size_scale = app_settings ? std::clamp(app_settings->road_label_size_scale, 0.65f, 2.5f) : 1.0f;
    const bool angle_along_road = app_settings ? app_settings->road_label_angle_along_road : true;
    const ImVec2 map_min = session.origin;
    const ImVec2 map_max(session.origin.x + session.size.x, session.origin.y + session.size.y);

    items.reserve(label_state->selections.size());
    for (const RoadLabelSelection& state : label_state->selections) {
        ImVec2 anchor, min, max;
        if (!roadLabelScreenBox(state, session, size_scale, anchor, min, max)) continue;
        RoadLabelLayoutItem item;
        item.state = &state;
        item.caption = state.label;
        item.anchor = anchor;
        item.center = ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
        item.half_size = ImVec2((max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f);
        item.angle_rad = angle_along_road ? normalizeRoadLabelAngle(state.angle_rad) : 0.0f;
        updateRoadLabelLayoutBox(item);
        items.push_back(std::move(item));
    }
    if (roadLabelCollisionMode(app_settings) != 0) {
        applyRoadLabelCandidateLayout(items, session, app_settings);
        applyRoadLabelOverlapLayout(items, map_min, map_max, app_settings);
    }
    return items;
}

bool handleRoadLabelRemovalClick(RoadLabelState& label_state, const MapCanvasSession& session, const AppSettings* app_settings) {
    if (!label_state.visible || label_state.selections.empty()) return false;
    const ImGuiIO& io = ImGui::GetIO();
    const bool remove_click =
        session.map_hovered &&
        !session.navigation_click_consumed &&
        io.KeyCtrl &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <= 36.0f;
    if (!remove_click) return false;

    const std::vector<RoadLabelLayoutItem> items = buildRoadLabelLayout(&label_state, session, app_settings);
    for (size_t i = items.size(); i > 0; --i) {
        const RoadLabelLayoutItem& item = items[i - 1];
        if (io.MousePos.x >= item.min.x && io.MousePos.x <= item.max.x && io.MousePos.y >= item.min.y && io.MousePos.y <= item.max.y) {
            auto erase_it = std::find_if(
                label_state.selections.begin(),
                label_state.selections.end(),
                [&](const RoadLabelSelection& state) { return &state == item.state; });
            if (erase_it != label_state.selections.end()) {
                const size_t erased_idx = (size_t)std::distance(label_state.selections.begin(), erase_it);
                label_state.selections.erase(erase_it);
                if (label_state.selected_idx == erased_idx) {
                    label_state.selected_idx = (size_t)-1;
                } else if (label_state.selected_idx != (size_t)-1 && label_state.selected_idx > erased_idx) {
                    label_state.selected_idx -= 1;
                }
            }
            return true;
        }
    }
    return false;
}

bool handleRoadLabelInspectorClick(RoadLabelState& label_state, const MapCanvasSession& session, const AppSettings* app_settings) {
    if (!label_state.visible || label_state.selections.empty()) return false;
    const ImGuiIO& io = ImGui::GetIO();
    const bool inspect_click =
        session.map_hovered &&
        !session.navigation_click_consumed &&
        !io.KeyCtrl &&
        !io.KeyAlt &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <= 36.0f;
    if (!inspect_click) return false;

    const std::vector<RoadLabelLayoutItem> items = buildRoadLabelLayout(&label_state, session, app_settings);
    for (size_t i = items.size(); i > 0; --i) {
        const RoadLabelLayoutItem& item = items[i - 1];
        if (io.MousePos.x < item.min.x || io.MousePos.x > item.max.x ||
            io.MousePos.y < item.min.y || io.MousePos.y > item.max.y) {
            continue;
        }
        auto selected_it = std::find_if(
            label_state.selections.begin(),
            label_state.selections.end(),
            [&](const RoadLabelSelection& state) { return &state == item.state; });
        if (selected_it == label_state.selections.end()) return false;
        label_state.selected_idx = (size_t)std::distance(label_state.selections.begin(), selected_it);
        label_state.inspector_open_requested = true;
        return true;
    }
    return false;
}

bool roadLabelClickShouldDeferToPrimaryMapTarget(const MapCanvasSession& session) {
    const MapHoverState& hover = session.hover_state;
    if (session.parcel_inspect_active && hover.inspect_parcel_idx != (size_t)-1) return true;
    if (session.zoning_inspect_active && hover.hovered_zone_idx != (size_t)-1) return true;
    if (hover.inspect_point_idx != (size_t)-1) return true;
    return false;
}

bool handleRoadLabelClickMode(const MapTabContext& ctx, const MapCanvasSession& session) {
    if (!ctx.app_settings || !ctx.app_settings->road_label_click_mode || !ctx.layers ||
        !ctx.road_label_state || !ctx.road_label_state->visible) {
        return false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const bool click_select =
        session.map_hovered &&
        !session.navigation_click_consumed &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        !io.KeyAlt &&
        !io.KeyCtrl &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <= 36.0f;
    if (!click_select) return false;

    const float tolerance_px = std::clamp(ctx.app_settings->road_label_pick_tolerance_px, 4.0f, 32.0f);
    int best_layer_idx = -1;
    size_t best_feature_idx = (size_t)-1;
    ImVec2 best_anchor = io.MousePos;
    ImVec2 best_anchor_lonlat = session.mouse_ll;
    float best_angle_rad = 0.0f;
    float best_dist_sq = std::numeric_limits<float>::max();
    auto pick_layers = [&](bool enabled_layers) {
        for (size_t layer_idx = 0; layer_idx < ctx.layers->size(); ++layer_idx) {
            const LayerDef& layer = (*ctx.layers)[layer_idx];
            if (layer.enabled != enabled_layers || !isRoadLabelLayer(layer)) continue;
            bool artifact_found = tryPickRoadLabelFromArtifact(
                ctx, session, layer_idx, tolerance_px, best_layer_idx, best_feature_idx, best_anchor, best_anchor_lonlat, best_angle_rad, best_dist_sq);
            if (!artifact_found) {
                if (const PolylineGeometryArtifact* selectable_artifact = selectableRoadLabelArtifactForLayer(ctx, layer_idx)) {
                    artifact_found = tryPickRoadLabelFromPolylineArtifact(
                        ctx,
                        session,
                        layer_idx,
                        *selectable_artifact,
                        tolerance_px,
                        best_layer_idx,
                        best_feature_idx,
                        best_anchor,
                        best_anchor_lonlat,
                        best_angle_rad,
                        best_dist_sq);
                }
            }
            if (!artifact_found && enabled_layers) {
                tryPickRoadLabelFromLayerFeatures(
                    ctx, session, layer_idx, tolerance_px, best_layer_idx, best_feature_idx, best_anchor, best_anchor_lonlat, best_angle_rad, best_dist_sq);
            }
        }
    };
    pick_layers(true);
    if (best_layer_idx < 0) {
        pick_layers(false);
    }
    if (best_layer_idx < 0 || best_feature_idx == (size_t)-1) {
        return false;
    }
    RoadLabelSelection label_selection;
    label_selection.label = resolveRoadLabelForPickedFeature(ctx, (size_t)best_layer_idx, best_feature_idx);
    label_selection.layer_idx = best_layer_idx;
    label_selection.feature_idx = best_feature_idx;
    label_selection.anchor_lonlat = best_anchor_lonlat;
    label_selection.angle_rad = best_angle_rad;
    auto existing = std::find_if(
        ctx.road_label_state->selections.begin(),
        ctx.road_label_state->selections.end(),
        [&](const RoadLabelSelection& state) {
            return state.layer_idx == best_layer_idx && state.feature_idx == best_feature_idx;
        });
    if (existing != ctx.road_label_state->selections.end()) {
        *existing = label_selection;
    } else {
        ctx.road_label_state->selections.push_back(std::move(label_selection));
    }
    if (ctx.target_indicator) {
        startTargetIndicator(*ctx.target_indicator, ImGui::GetTime(), best_anchor_lonlat.x, best_anchor_lonlat.y);
    }
    return true;
}

void drawRoadLabelPopups(const RoadLabelState* label_state, const MapCanvasSession& session, const AppSettings* app_settings) {
    if (!label_state || !label_state->visible || label_state->selections.empty() || !session.draw) return;
    const ImVec2 map_min = session.origin;
    const ImVec2 map_max(session.origin.x + session.size.x, session.origin.y + session.size.y);
    const float size_scale = app_settings ? std::clamp(app_settings->road_label_size_scale, 0.65f, 2.5f) : 1.0f;
    const ImVec2 pad(10.0f * size_scale, 6.0f * size_scale);
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize() * size_scale;
    const ImGuiIO& io = ImGui::GetIO();
    std::vector<RoadLabelLayoutItem> items = buildRoadLabelLayout(label_state, session, app_settings);

    session.draw->PushClipRect(map_min, map_max, true);
    for (const RoadLabelLayoutItem& item : items) {
        const bool hovered =
            io.MousePos.x >= item.min.x && io.MousePos.x <= item.max.x &&
            io.MousePos.y >= item.min.y && io.MousePos.y <= item.max.y;
        const float displaced_dx = item.center.x - item.anchor.x;
        const float displaced_dy = item.center.y - item.anchor.y;
        const bool displaced = displaced_dx * displaced_dx + displaced_dy * displaced_dy > 36.0f;
        ImVec2 box_pts[4] = {
            rotatePointAround(item.min, item.center, item.angle_rad),
            rotatePointAround(ImVec2(item.max.x, item.min.y), item.center, item.angle_rad),
            rotatePointAround(item.max, item.center, item.angle_rad),
            rotatePointAround(ImVec2(item.min.x, item.max.y), item.center, item.angle_rad)
        };
        if (displaced) {
            session.draw->AddLine(item.anchor, item.center, IM_COL32(255, 214, 82, 125), 1.2f);
        }
        session.draw->AddConvexPolyFilled(box_pts, 4, hovered ? IM_COL32(31, 40, 48, 240) : IM_COL32(18, 24, 30, 232));
        session.draw->AddPolyline(box_pts, 4, IM_COL32(255, 214, 82, 190), ImDrawFlags_Closed, 1.0f);
        const int text_vtx_start = session.draw->VtxBuffer.Size;
        if (font) {
            session.draw->AddText(font, font_size, ImVec2(item.min.x + pad.x, item.min.y + pad.y), IM_COL32(248, 250, 252, 245), item.caption.c_str());
        } else {
            session.draw->AddText(ImVec2(item.min.x + pad.x, item.min.y + pad.y), IM_COL32(248, 250, 252, 245), item.caption.c_str());
        }
        rotateDrawListVertices(session.draw, text_vtx_start, item.center, item.angle_rad);
    }
    session.draw->PopClipRect();
}

std::string trimCopy(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace((unsigned char)value[begin])) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace((unsigned char)value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

std::string toUpperAsciiCopy(std::string value) {
    for (char& c : value) c = (char)std::toupper((unsigned char)c);
    return value;
}

void drawMapTitleOverlay(const MapCanvasSession& session, const std::string& title, const std::string& source_label) {
    if (!session.draw) return;
    const std::string clean_title = trimCopy(title);
    const std::string clean_source = trimCopy(source_label);
    if (clean_title.empty() && clean_source.empty()) return;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (!draw) return;

    ImFont* font = getWorldsimMapTitleFont();
    if (!font) font = ImGui::GetFont();
    if (!font) return;
    const float title_font_size = 48.0f;
    const float source_font_size = ImGui::GetFontSize() * 1.0f;
    const ImVec2 title_size =
        clean_title.empty() ? ImVec2(0.0f, 0.0f) : font->CalcTextSizeA(title_font_size, FLT_MAX, 0.0f, clean_title.c_str());
    const std::string source_text = clean_source.empty() ? std::string() : ("Source: " + clean_source);
    const ImVec2 source_size =
        source_text.empty() ? ImVec2(0.0f, 0.0f) : font->CalcTextSizeA(source_font_size, FLT_MAX, 0.0f, source_text.c_str());
    const float content_w = std::max(title_size.x, source_size.x);
    const float content_h = title_size.y + (source_text.empty() ? 0.0f : (6.0f + source_size.y));
    const ImVec2 pad(18.0f, 12.0f);
    const ImVec2 box_min(
        session.origin.x + std::max(0.0f, (session.size.x - content_w) * 0.5f) - pad.x,
        session.origin.y + 14.0f);
    const ImVec2 box_max(box_min.x + content_w + pad.x * 2.0f, box_min.y + content_h + pad.y * 2.0f);

    draw->PushClipRect(session.origin, ImVec2(session.origin.x + session.size.x, session.origin.y + session.size.y), true);
    draw->AddRectFilled(box_min, box_max, IM_COL32(17, 24, 32, 205), 12.0f);
    draw->AddRect(box_min, box_max, IM_COL32(255, 255, 255, 72), 12.0f);

    float y = box_min.y + pad.y;
    if (!clean_title.empty()) {
        const float title_x = session.origin.x + session.size.x * 0.5f - title_size.x * 0.5f;
        draw->AddText(font, title_font_size, ImVec2(title_x + 1.0f, y + 1.0f), IM_COL32(0, 0, 0, 150), clean_title.c_str());
        draw->AddText(font, title_font_size, ImVec2(title_x, y), IM_COL32(250, 250, 250, 245), clean_title.c_str());
        y += title_size.y + 6.0f;
    }
    if (!source_text.empty()) {
        const float source_x = session.origin.x + session.size.x * 0.5f - source_size.x * 0.5f;
        draw->AddText(font, source_font_size, ImVec2(source_x + 1.0f, y + 1.0f), IM_COL32(0, 0, 0, 140), source_text.c_str());
        draw->AddText(font, source_font_size, ImVec2(source_x, y), IM_COL32(220, 226, 232, 235), source_text.c_str());
    }
    draw->PopClipRect();
}

struct GradientLegendItem {
    std::string label;
    std::string field;
    ApproxHistogram hist;
};

struct CategoricalLegendItem {
    std::string label;
    ImVec4 color;
};

std::string compactLegendNumber(double value) {
    std::ostringstream os;
    if (std::abs(value) >= 1000000000.0) os << std::fixed << std::setprecision(1) << (value / 1000000000.0) << "B";
    else if (std::abs(value) >= 1000000.0) os << std::fixed << std::setprecision(1) << (value / 1000000.0) << "M";
    else if (std::abs(value) >= 1000.0) os << std::fixed << std::setprecision(0) << (value / 1000.0) << "K";
    else if (std::abs(value) >= 100.0) os << std::fixed << std::setprecision(0) << value;
    else os << std::fixed << std::setprecision(1) << value;
    return os.str();
}

std::string truncateLegendLabel(const std::string& label, size_t max_chars = 34) {
    if (label.size() <= max_chars) return label;
    if (max_chars <= 3) return label.substr(0, max_chars);
    return label.substr(0, max_chars - 3) + "...";
}

bool layerGradientEnabled(const MapTabContext& ctx, size_t layer_idx) {
    return ctx.layer_heatmap_use_gradient &&
        layer_idx < ctx.layer_heatmap_use_gradient->size() &&
        (*ctx.layer_heatmap_use_gradient)[layer_idx];
}

std::vector<double> collectLegendNumericSamples(const LayerDef& layer, size_t max_samples = 5000) {
    std::vector<double> values;
    if (layer.heatmap_field.empty() || layer.features.empty()) return values;
    const size_t stride = std::max<size_t>(1, layer.features.size() / max_samples);
    values.reserve(std::min(max_samples, layer.features.size()));
    for (size_t i = 0; i < layer.features.size(); i += stride) {
        float v = 0.0f;
        if (tryGetFeaturePropertyFloat(layer.features[i], layer.heatmap_field, v) && std::isfinite(v)) {
            values.push_back((double)v);
        }
    }
    return values;
}

std::vector<GradientLegendItem> buildGradientLegendItems(const MapTabContext& ctx) {
    std::vector<GradientLegendItem> items;
    if (!ctx.layers) return items;
    for (size_t li = 0; li < ctx.layers->size() && items.size() < 3; ++li) {
        const LayerDef& layer = (*ctx.layers)[li];
        if (!layer.enabled || layer.heatmap_field.empty() || !layerGradientEnabled(ctx, li)) continue;
        const float clip_pct =
            ctx.layer_heatmap_percentile_clip && li < ctx.layer_heatmap_percentile_clip->size()
                ? (*ctx.layer_heatmap_percentile_clip)[li]
                : ctx.heatmap_percentile_clip;
        ApproxHistogram hist = buildApproxHistogram(collectLegendNumericSamples(layer), clip_pct);
        if (!hist.rangeValid()) continue;
        items.push_back(GradientLegendItem{
            layer.name.empty() ? layer.file : layer.name,
            layer.heatmap_field,
            std::move(hist)
        });
    }
    return items;
}

std::vector<CategoricalLegendItem> buildZoningLegendItems(const MapTabContext& ctx) {
    std::vector<CategoricalLegendItem> items;
    if (!ctx.layers || ctx.zoning_layer_idx < 0 || (size_t)ctx.zoning_layer_idx >= ctx.layers->size()) return items;
    const LayerDef& layer = (*ctx.layers)[(size_t)ctx.zoning_layer_idx];
    if (!layer.enabled || !ctx.zoning_zone_enabled || !ctx.zoning_zone_color) return items;
    std::vector<std::string> keys;
    for (const LayerDef::FeatureRecord& fg : layer.features) {
        const std::string key = zoningClassKey(fg);
        if (key.empty()) continue;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
        if (keys.size() >= 10) break;
    }
    for (const std::string& key : keys) {
        auto color_it = ctx.zoning_zone_color->find(key);
        if (color_it == ctx.zoning_zone_color->end()) continue;
        std::string label = key;
        if (ctx.zoning_metadata) {
            auto meta_it = ctx.zoning_metadata->find(key);
            if (meta_it != ctx.zoning_metadata->end() && !meta_it->second.label.empty()) label = meta_it->second.label;
        }
        items.push_back(CategoricalLegendItem{label, color_it->second});
    }
    return items;
}

void drawGradientBar(ImDrawList* draw, const ImVec2& min, const ImVec2& max) {
    constexpr int kSteps = 40;
    const float w = max.x - min.x;
    for (int i = 0; i < kSteps; ++i) {
        const float t0 = (float)i / (float)kSteps;
        const float t1 = (float)(i + 1) / (float)kSteps;
        const ImVec2 p0(min.x + w * t0, min.y);
        const ImVec2 p1(min.x + w * t1 + 0.5f, max.y);
        draw->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(heatColor((t0 + t1) * 0.5f)));
    }
    draw->AddRect(min, max, IM_COL32(255, 255, 255, 82), 3.0f);
}

void drawMapLegendOverlay(const MapCanvasSession& session, const MapTabContext& ctx, int position) {
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (!draw) return;

    std::vector<const QueryMapLayer*> visible_layers;
    if (ctx.query_layers) {
        visible_layers.reserve(ctx.query_layers->size());
        for (const QueryMapLayer& layer : *ctx.query_layers) {
            if (!layer.enabled) continue;
            visible_layers.push_back(&layer);
        }
    }
    const std::vector<GradientLegendItem> gradient_items = buildGradientLegendItems(ctx);
    const std::vector<CategoricalLegendItem> zoning_items = buildZoningLegendItems(ctx);
    if (visible_layers.empty() && gradient_items.empty() && zoning_items.empty()) return;

    ImFont* font = ImGui::GetFont();
    if (!font) return;
    const float title_font_size = ImGui::GetFontSize() * 1.02f;
    const float row_font_size = ImGui::GetFontSize() * 0.96f;
    const float swatch = 12.0f;
    const float gradient_w = 168.0f;
    const float gradient_h = 12.0f;
    const float row_gap = 6.0f;
    const float pad_x = 14.0f;
    const float pad_y = 12.0f;
    const char* title = "Legend";
    const ImVec2 title_size = font->CalcTextSizeA(title_font_size, FLT_MAX, 0.0f, title);
    float content_w = title_size.x;
    float content_h = title_size.y;
    for (const QueryMapLayer* layer : visible_layers) {
        std::string label = layer->name.empty() ? "Query Layer" : layer->name;
        if (layer->row_count > 0) label += " (" + std::to_string(layer->row_count) + ")";
        const ImVec2 text_size = font->CalcTextSizeA(row_font_size, FLT_MAX, 0.0f, label.c_str());
        content_w = std::max(content_w, swatch + 10.0f + text_size.x);
        content_h += row_gap + std::max(swatch, text_size.y);
    }
    for (const GradientLegendItem& item : gradient_items) {
        const std::string label = truncateLegendLabel(item.label);
        const std::string range =
            compactLegendNumber(item.hist.min_value) + " - " +
            compactLegendNumber((item.hist.min_value + item.hist.clipped_max_value) * 0.5) + " - " +
            compactLegendNumber(item.hist.clipped_max_value);
        const ImVec2 label_size = font->CalcTextSizeA(row_font_size, FLT_MAX, 0.0f, label.c_str());
        const ImVec2 range_size = font->CalcTextSizeA(row_font_size * 0.88f, FLT_MAX, 0.0f, range.c_str());
        content_w = std::max(content_w, std::max(gradient_w, std::max(label_size.x, range_size.x)));
        content_h += row_gap + label_size.y + 4.0f + gradient_h + 3.0f + range_size.y;
    }
    if (!zoning_items.empty()) {
        const char* zoning_title = "Zoning";
        const ImVec2 zoning_title_size = font->CalcTextSizeA(row_font_size, FLT_MAX, 0.0f, zoning_title);
        content_w = std::max(content_w, zoning_title_size.x);
        content_h += row_gap + zoning_title_size.y;
    }
    for (const CategoricalLegendItem& item : zoning_items) {
        const std::string label = truncateLegendLabel(item.label);
        const ImVec2 text_size = font->CalcTextSizeA(row_font_size, FLT_MAX, 0.0f, label.c_str());
        content_w = std::max(content_w, swatch + 10.0f + text_size.x);
        content_h += row_gap + std::max(swatch, text_size.y);
    }

    const ImVec2 box_size(content_w + pad_x * 2.0f, content_h + pad_y * 2.0f);
    ImVec2 box_min(session.origin.x + 18.0f, session.origin.y + 22.0f);
    if (position == 1) {
        box_min = ImVec2(session.origin.x + session.size.x - box_size.x - 18.0f, session.origin.y + 22.0f);
    } else if (position == 2) {
        box_min = ImVec2(session.origin.x + 18.0f, session.origin.y + session.size.y - box_size.y - 18.0f);
    } else if (position == 3) {
        box_min = ImVec2(
            session.origin.x + session.size.x - box_size.x - 18.0f,
            session.origin.y + session.size.y - box_size.y - 18.0f);
    }
    const ImVec2 box_max(box_min.x + box_size.x, box_min.y + box_size.y);
    draw->PushClipRect(session.origin, ImVec2(session.origin.x + session.size.x, session.origin.y + session.size.y), true);
    draw->AddRectFilled(box_min, box_max, IM_COL32(17, 24, 32, 214), 12.0f);
    draw->AddRect(box_min, box_max, IM_COL32(255, 255, 255, 72), 12.0f);

    float y = box_min.y + pad_y;
    draw->AddText(font, title_font_size, ImVec2(box_min.x + pad_x, y), IM_COL32(245, 248, 250, 245), title);
    y += title_size.y + row_gap;
    for (const QueryMapLayer* layer : visible_layers) {
        std::string label = layer->name.empty() ? "Query Layer" : layer->name;
        if (layer->row_count > 0) label += " (" + std::to_string(layer->row_count) + ")";
        const ImVec2 text_size = font->CalcTextSizeA(row_font_size, FLT_MAX, 0.0f, label.c_str());
        const float row_h = std::max(swatch, text_size.y);
        const ImVec2 swatch_min(box_min.x + pad_x, y + (row_h - swatch) * 0.5f);
        const ImVec2 swatch_max(swatch_min.x + swatch, swatch_min.y + swatch);
        const ImU32 fill = ImGui::ColorConvertFloat4ToU32(
            ImVec4(layer->color[0], layer->color[1], layer->color[2], layer->color[3]));
        const ImU32 outline = ImGui::ColorConvertFloat4ToU32(
            ImVec4(layer->outline_color[0], layer->outline_color[1], layer->outline_color[2], layer->outline_color[3]));
        draw->AddRectFilled(swatch_min, swatch_max, fill, 3.0f);
        draw->AddRect(swatch_min, swatch_max, outline, 3.0f);
        draw->AddText(
            font,
            row_font_size,
            ImVec2(swatch_max.x + 10.0f, y + (row_h - text_size.y) * 0.5f),
            IM_COL32(232, 236, 240, 240),
            label.c_str());
        y += row_h + row_gap;
    }
    for (const GradientLegendItem& item : gradient_items) {
        const std::string label = truncateLegendLabel(item.label);
        draw->AddText(
            font,
            row_font_size,
            ImVec2(box_min.x + pad_x, y),
            IM_COL32(232, 236, 240, 240),
            label.c_str());
        y += font->CalcTextSizeA(row_font_size, FLT_MAX, 0.0f, label.c_str()).y + 4.0f;
        const ImVec2 grad_min(box_min.x + pad_x, y);
        const ImVec2 grad_max(grad_min.x + gradient_w, grad_min.y + gradient_h);
        drawGradientBar(draw, grad_min, grad_max);
        y += gradient_h + 3.0f;
        const std::string range =
            compactLegendNumber(item.hist.min_value) + "   " +
            compactLegendNumber((item.hist.min_value + item.hist.clipped_max_value) * 0.5) + "   " +
            compactLegendNumber(item.hist.clipped_max_value);
        draw->AddText(
            font,
            row_font_size * 0.88f,
            ImVec2(box_min.x + pad_x, y),
            IM_COL32(205, 212, 220, 230),
            range.c_str());
        y += font->CalcTextSizeA(row_font_size * 0.88f, FLT_MAX, 0.0f, range.c_str()).y + row_gap;
    }
    if (!zoning_items.empty()) {
        draw->AddText(
            font,
            row_font_size,
            ImVec2(box_min.x + pad_x, y),
            IM_COL32(245, 248, 250, 235),
            "Zoning");
        y += font->CalcTextSizeA(row_font_size, FLT_MAX, 0.0f, "Zoning").y + row_gap;
    }
    for (const CategoricalLegendItem& item : zoning_items) {
        const std::string label = truncateLegendLabel(item.label);
        const ImVec2 text_size = font->CalcTextSizeA(row_font_size, FLT_MAX, 0.0f, label.c_str());
        const float row_h = std::max(swatch, text_size.y);
        const ImVec2 swatch_min(box_min.x + pad_x, y + (row_h - swatch) * 0.5f);
        const ImVec2 swatch_max(swatch_min.x + swatch, swatch_min.y + swatch);
        draw->AddRectFilled(swatch_min, swatch_max, ImGui::ColorConvertFloat4ToU32(item.color), 3.0f);
        draw->AddRect(swatch_min, swatch_max, IM_COL32(255, 255, 255, 82), 3.0f);
        draw->AddText(
            font,
            row_font_size,
            ImVec2(swatch_max.x + 10.0f, y + (row_h - text_size.y) * 0.5f),
            IM_COL32(232, 236, 240, 240),
            label.c_str());
        y += row_h + row_gap;
    }
    draw->PopClipRect();
}

MapCornerControlState hitTestMapCornerControls(const MapTabContext& ctx, const MapCanvasSession& session) {
    MapCornerControlState state;
    constexpr float button = 34.0f;
    ImVec2 fullscreen_min;
    ImVec2 camera_min;
    ImVec2 video_min;
    mapCornerControlPositions(session, fullscreen_min, camera_min, video_min);

    ImGui::SetCursorScreenPos(fullscreen_min);
    ImGui::InvisibleButton("##map_fullscreen", ImVec2(button, button));
    state.fullscreen_hovered = ImGui::IsItemHovered();
    if (state.fullscreen_hovered) ImGui::SetTooltip("%s", ctx.map_fullscreen ? "Exit fullscreen" : "Fullscreen map");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && ctx.toggle_map_fullscreen) ctx.toggle_map_fullscreen();

    ImGui::SetCursorScreenPos(camera_min);
    ImGui::InvisibleButton("##map_snapshot", ImVec2(button, button));
    state.snapshot_hovered = ImGui::IsItemHovered();
    if (state.snapshot_hovered) ImGui::SetTooltip("Snapshot");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && ctx.request_snapshot) ctx.request_snapshot();

    ImGui::SetCursorScreenPos(video_min);
    ImGui::InvisibleButton("##map_video_record", ImVec2(button, button));
    state.video_hovered = ImGui::IsItemHovered();
    const bool recording = ctx.video_recording_active && ctx.video_recording_active();
    if (state.video_hovered) ImGui::SetTooltip("%s", recording ? "Stop recording" : "Record video");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && ctx.toggle_video_recording) ctx.toggle_video_recording();

    state.hovered = state.fullscreen_hovered || state.snapshot_hovered || state.video_hovered;
    return state;
}

void drawMapCornerControlsVisual(const MapTabContext& ctx, const MapCanvasSession& session, const MapCornerControlState& state) {
    if (!session.draw) return;
    ImVec2 fullscreen_min;
    ImVec2 camera_min;
    ImVec2 video_min;
    mapCornerControlPositions(session, fullscreen_min, camera_min, video_min);
    drawMapFullscreenIcon(session.draw, fullscreen_min, state.fullscreen_hovered);
    drawMapCameraIcon(session.draw, camera_min, state.snapshot_hovered);
    const bool recording = ctx.video_recording_active && ctx.video_recording_active();
    drawMapVideoIcon(session.draw, video_min, state.video_hovered, recording);
}

bool isZoningPolygonLayerForGpu(const LayerDef& layer) {
    if (layer.scale == "point") return false;
    if (layer.category == LayerDef::Category::Zoning) return true;
    std::string file_lower = layer.file;
    std::string name_lower = layer.name;
    std::transform(file_lower.begin(), file_lower.end(), file_lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return file_lower.find("zoning") != std::string::npos ||
           name_lower.find("zoning") != std::string::npos;
}

bool isGenericPolygonLayerForGpu(const LayerDef& layer) {
    return !layerUsesPointGeometry(layer) && !layerUsesPolylineGeometry(layer);
}
}

void drawMapTabWindow(const MapTabContext& ctx) {
    if (!ctx.root || !ctx.app_settings || !ctx.duckdb_analytics || !ctx.center_lon || !ctx.center_lat || !ctx.zoom ||
        !ctx.layers || !ctx.layer_spatial || !ctx.map_filter_state || !ctx.query_layers || !ctx.real_property_by_blocklot ||
        !ctx.zoning_metadata || !ctx.zoning_zone_enabled || !ctx.zoning_zone_color || !ctx.parcel_selection ||
        !ctx.selected_parcel_ids || !ctx.show_selected_zone_details || !ctx.selected_zone_idx || !ctx.element_info_state ||
        !ctx.layer_fill_enabled || !ctx.layer_hover_enabled || !ctx.layer_inspect_enabled || !ctx.layer_heatmap_enabled ||
        !ctx.layer_heatmap_algo || !ctx.layer_heatmap_max_zoom || !ctx.layer_parcel_detail_min_zoom || !ctx.layer_heatmap_cell_px ||
        !ctx.layer_heatmap_bandwidth_px || !ctx.layer_heatmap_blur_sigma_px || !ctx.layer_heatmap_percentile_clip ||
        !ctx.layer_heatmap_zoom_adaptive_bandwidth || !ctx.layer_heatmap_multires_enabled || !ctx.layer_heatmap_multires_blend ||
        !ctx.layer_heatmap_use_gradient || !ctx.layer_choropleth_gamma || !ctx.layer_normalize_mode ||
        !ctx.parcel_jurisdiction_filter_state || !ctx.parcel_vac_notice_by_feature || !ctx.parcel_vac_rehab_by_feature ||
        !ctx.parcel_tax_lien_by_feature || !ctx.parcel_tax_sale_by_feature || !ctx.parcel_tax_lien_amount_by_feature ||
        !ctx.parcel_tax_sale_amount_by_feature || !ctx.unified_parcels || !ctx.heatmap_runtime ||
        !ctx.lazy_tile_download || !ctx.topo_tiles_available_cached || !ctx.topo_vector_available_cached ||
        !ctx.basemap_source_has_any_files_cached || !ctx.tile_root_dir_cached || !ctx.basemap_availability_last_check ||
        !ctx.prof_tiles_drawn_frame || !ctx.prof_features_considered_frame || !ctx.prof_features_drawn_frame ||
        !ctx.prof_tile_ms_last || !ctx.prof_layer_ms_last || !ctx.prof_heatmap_ms_last || !ctx.prof_heat_samples_last ||
        !ctx.prof_heatmap_gpu_splat_active || !ctx.prof_heatmap_high_quality || !ctx.prof_heatmap_cache_valid ||
        !ctx.prof_heatmap_texture_resident || !ctx.prof_heatmap_async_inflight || !ctx.prof_heatmap_cache_key ||
        !ctx.prof_heatmap_texture_cache_entries || !ctx.visible_vacant_parcels_last_frame || !ctx.prof_overlay_ms_last ||
        !ctx.render_fill_attempts_last_frame || !ctx.render_fill_success_last_frame || !ctx.render_fill_no_triangles_last_frame ||
        !ctx.render_fill_bad_indices_last_frame || !ctx.time_cube_service || !ctx.time_cube_ui_result || !ctx.time_cube_ui_loaded ||
        !ctx.time_cube_ui_status || !ctx.time_cube_ui_mutex || !ctx.time_cube_ui_worker || !ctx.time_cube_ui_running ||
        !ctx.time_cube_ui_done || !ctx.time_cube_selected || !ctx.time_cube_year_min || !ctx.time_cube_year_max ||
        !ctx.time_cube_normalize_mode || !ctx.time_cube_show_excluded || !ctx.policy_hierarchy || !ctx.policy_hierarchy_error ||
        !ctx.policy_hierarchy_query || !ctx.policy_hierarchy_scope || !ctx.public_servant_roster || !ctx.people_pay_cached_query ||
        !ctx.people_pay_cached_scope || !ctx.people_pay_cache_matched_count || !ctx.people_pay_visible_rows ||
        !ctx.people_pay_cache_rebuilds || !ctx.people_pay_rendered_rows_last || !ctx.policy_viz_root ||
        !ctx.policy_viz_cached_query || !ctx.policy_viz_cached_scope || !ctx.policy_viz_cached_metric ||
        !ctx.policy_viz_metric || !ctx.policy_viz_cache_rebuilds || !ctx.policy_viz_node_count) {
        return;
    }

    clearParcelGpuDrawState();

    ImGui::SetNextWindowPos(ImVec2(ctx.map_x, ctx.layout_margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ctx.map_w, ctx.main_panel_h), ImGuiCond_Always);
    ImGui::Begin("Map", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    MapCanvasSessionContext map_canvas_ctx{
                ctx.center_lon,
                ctx.center_lat,
                ctx.zoom,
                ctx.camera_animation,
                ctx.target_indicator,
                ctx.min_zoom,
                ctx.max_zoom,
                ctx.max_internal_math_zoom,
                ctx.root,
                ctx.app_settings,
                ctx.lazy_tile_download,
                ctx.topo_tiles_available_cached,
                ctx.topo_vector_available_cached,
                ctx.basemap_source_has_any_files_cached,
                ctx.tile_root_dir_cached,
                ctx.basemap_availability_last_check,
                ctx.max_native_tile_zoom,
                ctx.layers,
                ctx.point_geometry_artifacts,
                ctx.polygon_geometry_artifacts,
                ctx.parcel_render_blob,
                ctx.layer_spatial,
                ctx.layer_hover_enabled,
                ctx.layer_inspect_enabled,
                ctx.active_hover_layer_idx,
                ctx.active_click_layer_idx,
                ctx.parcel_layer_idx,
                ctx.zoning_layer_idx,
                ctx.vacant_notice_layer_idx,
                ctx.vacant_rehab_layer_idx,
                ctx.tax_lien_layer_idx,
                ctx.tax_sale_layer_idx,
                ctx.prof_tiles_drawn_frame,
                ctx.prof_tile_ms_last,
                ctx.persistent_projection_cache,
                ctx.persistent_projection_generation,
                ctx.projection_generation,
                ctx.prof_projection_world_ring_cache_entries,
                ctx.prof_projection_world_extent_cache_entries,
                ctx.prof_projection_cache_generation
            };
            MapCanvasSession map_canvas_session = beginMapCanvasSession(map_canvas_ctx);
            const MapCornerControlState map_corner_controls = hitTestMapCornerControls(ctx, map_canvas_session);
            drawMapFpsOverlay(map_canvas_session);

            if (ctx.parcel_layer_idx >= 0 &&
                (size_t)ctx.parcel_layer_idx < ctx.layers->size() &&
                (*ctx.layers)[(size_t)ctx.parcel_layer_idx].enabled) {
                const ImGuiIO& io = ImGui::GetIO();
                const ImVec2 fb_scale = io.DisplayFramebufferScale;
                ParcelGpuDrawConfig parcel_draw_cfg;
                parcel_draw_cfg.active = true;
                parcel_draw_cfg.math_zoom = map_canvas_session.math_zoom;
                parcel_draw_cfg.zoom_scale = (float)(map_canvas_session.zoom_scale * std::max(1.0f, fb_scale.x));
                parcel_draw_cfg.center_lonlat = ImVec2(
                    ctx.center_lon ? (float)*ctx.center_lon : 0.0f,
                    ctx.center_lat ? (float)*ctx.center_lat : 0.0f);
                parcel_draw_cfg.center_world = map_canvas_session.center_world;
                parcel_draw_cfg.viewport_origin =
                    ImVec2(map_canvas_session.origin.x * fb_scale.x, map_canvas_session.origin.y * fb_scale.y);
                parcel_draw_cfg.viewport_size =
                    ImVec2(map_canvas_session.size.x * fb_scale.x, map_canvas_session.size.y * fb_scale.y);
                parcel_draw_cfg.framebuffer_size =
                    ImVec2(io.DisplaySize.x * fb_scale.x, io.DisplaySize.y * fb_scale.y);
                parcel_draw_cfg.view_min_lon = map_canvas_session.view_min_lon;
                parcel_draw_cfg.view_min_lat = map_canvas_session.view_min_lat;
                parcel_draw_cfg.view_max_lon = map_canvas_session.view_max_lon;
                parcel_draw_cfg.view_max_lat = map_canvas_session.view_max_lat;
                configureParcelGpuDrawState(parcel_draw_cfg);
            }
            bool any_zoning_gpu_layer_enabled = false;
            bool any_point_gpu_layer_enabled = false;
            bool any_polyline_gpu_layer_enabled = false;
            const ImGuiIO& io = ImGui::GetIO();
            const ImVec2 fb_scale = io.DisplayFramebufferScale;
            for (size_t layer_idx = 0; layer_idx < ctx.layers->size(); ++layer_idx) {
                const LayerDef& layer = (*ctx.layers)[layer_idx];
                if (!layer.enabled || !isGenericPolygonLayerForGpu(layer) || (int)layer_idx == ctx.parcel_layer_idx) {
                    clearZoningGpuDrawState(layer_idx);
                } else {
                    any_zoning_gpu_layer_enabled = true;
                    ParcelGpuDrawConfig zoning_draw_cfg;
                    zoning_draw_cfg.active = true;
                    zoning_draw_cfg.math_zoom = map_canvas_session.math_zoom;
                    zoning_draw_cfg.zoom_scale = (float)(map_canvas_session.zoom_scale * std::max(1.0f, fb_scale.x));
                    zoning_draw_cfg.center_lonlat = ImVec2(
                        ctx.center_lon ? (float)*ctx.center_lon : 0.0f,
                        ctx.center_lat ? (float)*ctx.center_lat : 0.0f);
                    zoning_draw_cfg.center_world = map_canvas_session.center_world;
                    zoning_draw_cfg.viewport_origin =
                        ImVec2(map_canvas_session.origin.x * fb_scale.x, map_canvas_session.origin.y * fb_scale.y);
                    zoning_draw_cfg.viewport_size =
                        ImVec2(map_canvas_session.size.x * fb_scale.x, map_canvas_session.size.y * fb_scale.y);
                    zoning_draw_cfg.framebuffer_size =
                        ImVec2(io.DisplaySize.x * fb_scale.x, io.DisplaySize.y * fb_scale.y);
                    zoning_draw_cfg.view_min_lon = map_canvas_session.view_min_lon;
                    zoning_draw_cfg.view_min_lat = map_canvas_session.view_min_lat;
                    zoning_draw_cfg.view_max_lon = map_canvas_session.view_max_lon;
                    zoning_draw_cfg.view_max_lat = map_canvas_session.view_max_lat;
                    configureZoningGpuDrawState(layer_idx, zoning_draw_cfg);
                }

                if (!layer.enabled || !layerUsesPointGeometry(layer) || (int)layer_idx == ctx.crime_nibrs_layer_idx) {
                    clearPointLayerGpuDrawState(layer_idx);
                } else {
                    any_point_gpu_layer_enabled = true;
                    ParcelGpuDrawConfig point_draw_cfg;
                    point_draw_cfg.active = true;
                    point_draw_cfg.math_zoom = map_canvas_session.math_zoom;
                    point_draw_cfg.zoom_scale = (float)(map_canvas_session.zoom_scale * std::max(1.0f, fb_scale.x));
                    point_draw_cfg.center_lonlat = ImVec2(
                        ctx.center_lon ? (float)*ctx.center_lon : 0.0f,
                        ctx.center_lat ? (float)*ctx.center_lat : 0.0f);
                    point_draw_cfg.center_world = map_canvas_session.center_world;
                    point_draw_cfg.viewport_origin =
                        ImVec2(map_canvas_session.origin.x * fb_scale.x, map_canvas_session.origin.y * fb_scale.y);
                    point_draw_cfg.viewport_size =
                        ImVec2(map_canvas_session.size.x * fb_scale.x, map_canvas_session.size.y * fb_scale.y);
                    point_draw_cfg.framebuffer_size =
                        ImVec2(io.DisplaySize.x * fb_scale.x, io.DisplaySize.y * fb_scale.y);
                    point_draw_cfg.view_min_lon = map_canvas_session.view_min_lon;
                    point_draw_cfg.view_min_lat = map_canvas_session.view_min_lat;
                    point_draw_cfg.view_max_lon = map_canvas_session.view_max_lon;
                    point_draw_cfg.view_max_lat = map_canvas_session.view_max_lat;
                    configurePointLayerGpuDrawState(layer_idx, point_draw_cfg);
                }

                if (!layer.enabled || !layerUsesPolylineGeometry(layer)) {
                    clearPolylineLayerGpuDrawState(layer_idx);
                } else {
                    any_polyline_gpu_layer_enabled = true;
                    ParcelGpuDrawConfig polyline_draw_cfg;
                    polyline_draw_cfg.active = true;
                    polyline_draw_cfg.math_zoom = map_canvas_session.math_zoom;
                    polyline_draw_cfg.zoom_scale = (float)(map_canvas_session.zoom_scale * std::max(1.0f, fb_scale.x));
                    polyline_draw_cfg.center_world = map_canvas_session.center_world;
                    polyline_draw_cfg.viewport_origin =
                        ImVec2(map_canvas_session.origin.x * fb_scale.x, map_canvas_session.origin.y * fb_scale.y);
                    polyline_draw_cfg.viewport_size =
                        ImVec2(map_canvas_session.size.x * fb_scale.x, map_canvas_session.size.y * fb_scale.y);
                    polyline_draw_cfg.framebuffer_size =
                        ImVec2(io.DisplaySize.x * fb_scale.x, io.DisplaySize.y * fb_scale.y);
                    polyline_draw_cfg.view_min_lon = map_canvas_session.view_min_lon;
                    polyline_draw_cfg.view_min_lat = map_canvas_session.view_min_lat;
                    polyline_draw_cfg.view_max_lon = map_canvas_session.view_max_lon;
                    polyline_draw_cfg.view_max_lat = map_canvas_session.view_max_lat;
                    configurePolylineLayerGpuDrawState(layer_idx, polyline_draw_cfg);
                }
            }
            if (!any_zoning_gpu_layer_enabled) {
                clearAllZoningGpuDrawStates();
            }
            if (!any_point_gpu_layer_enabled) {
                clearAllPointLayerGpuDrawStates();
            }
            if (!any_polyline_gpu_layer_enabled) {
                clearAllPolylineLayerGpuDrawStates();
            }
            if (ctx.crime_nibrs_layer_idx >= 0 &&
                (size_t)ctx.crime_nibrs_layer_idx < ctx.layers->size() &&
                (*ctx.layers)[(size_t)ctx.crime_nibrs_layer_idx].enabled) {
                const ImGuiIO& io = ImGui::GetIO();
                const ImVec2 fb_scale = io.DisplayFramebufferScale;
                ParcelGpuDrawConfig crime_draw_cfg;
                crime_draw_cfg.active = true;
                crime_draw_cfg.math_zoom = map_canvas_session.math_zoom;
                crime_draw_cfg.zoom_scale = (float)(map_canvas_session.zoom_scale * std::max(1.0f, fb_scale.x));
                crime_draw_cfg.center_world = map_canvas_session.center_world;
                crime_draw_cfg.viewport_origin =
                    ImVec2(map_canvas_session.origin.x * fb_scale.x, map_canvas_session.origin.y * fb_scale.y);
                crime_draw_cfg.viewport_size =
                    ImVec2(map_canvas_session.size.x * fb_scale.x, map_canvas_session.size.y * fb_scale.y);
                crime_draw_cfg.framebuffer_size =
                    ImVec2(io.DisplaySize.x * fb_scale.x, io.DisplaySize.y * fb_scale.y);
                crime_draw_cfg.view_min_lon = map_canvas_session.view_min_lon;
                crime_draw_cfg.view_min_lat = map_canvas_session.view_min_lat;
                crime_draw_cfg.view_max_lon = map_canvas_session.view_max_lon;
                crime_draw_cfg.view_max_lat = map_canvas_session.view_max_lat;
                configureCrimePointGpuDrawState(crime_draw_cfg);
            } else {
                clearCrimePointGpuDrawState();
            }

            map_canvas_session.map_hovered = map_canvas_session.map_hovered && !map_corner_controls.hovered;
            refreshMapCanvasHoverState(map_canvas_session, map_canvas_ctx);
            const bool road_label_removal_consumed = ctx.road_label_state
                ? handleRoadLabelRemovalClick(*ctx.road_label_state, map_canvas_session, ctx.app_settings)
                : false;
            const bool road_label_inspector_consumed =
                !road_label_removal_consumed && ctx.road_label_state
                    ? handleRoadLabelInspectorClick(*ctx.road_label_state, map_canvas_session, ctx.app_settings)
                    : false;
            if (road_label_removal_consumed || road_label_inspector_consumed) {
                map_canvas_session.map_hovered = false;
            }
            if (ctx.hover_debug_state) {
                std::lock_guard<std::mutex> lk(ctx.hover_debug_state->mutex);
                ctx.hover_debug_state->map_hovered = map_canvas_session.map_hovered;
                ctx.hover_debug_state->mouse_screen_x = ImGui::GetIO().MousePos.x;
                ctx.hover_debug_state->mouse_screen_y = ImGui::GetIO().MousePos.y;
                ctx.hover_debug_state->mouse_lon = map_canvas_session.mouse_ll.x;
                ctx.hover_debug_state->mouse_lat = map_canvas_session.mouse_ll.y;
                ctx.hover_debug_state->hovered_parcel = map_canvas_session.hover_state.hovered_parcel_idx != (size_t)-1;
                ctx.hover_debug_state->hovered_parcel_layer_idx = map_canvas_session.hover_state.hovered_parcel_layer_idx;
                ctx.hover_debug_state->hovered_parcel_idx = map_canvas_session.hover_state.hovered_parcel_idx;
                ctx.hover_debug_state->hovered_parcel_entity_id = map_canvas_session.hover_state.hovered_parcel_entity_id;
                ctx.hover_debug_state->hovered_parcel_geometry_entity_id = map_canvas_session.hover_state.hovered_parcel_geometry_entity_id;
                ctx.hover_debug_state->inspect_parcel = map_canvas_session.hover_state.inspect_parcel_idx != (size_t)-1;
                ctx.hover_debug_state->inspect_parcel_layer_idx = map_canvas_session.hover_state.inspect_parcel_layer_idx;
                ctx.hover_debug_state->inspect_parcel_idx = map_canvas_session.hover_state.inspect_parcel_idx;
                ctx.hover_debug_state->inspect_parcel_entity_id = map_canvas_session.hover_state.inspect_parcel_entity_id;
                ctx.hover_debug_state->inspect_parcel_geometry_entity_id = map_canvas_session.hover_state.inspect_parcel_geometry_entity_id;
                ctx.hover_debug_state->hovered_zone = map_canvas_session.hover_state.hovered_zone_idx != (size_t)-1;
                ctx.hover_debug_state->hovered_zone_idx = map_canvas_session.hover_state.hovered_zone_idx;
                ctx.hover_debug_state->hovered_point = map_canvas_session.hover_state.hovered_point != nullptr;
                ctx.hover_debug_state->hovered_point_idx = map_canvas_session.hover_state.hovered_point_idx;
                ctx.hover_debug_state->hovered_point_layer_idx = map_canvas_session.hover_state.hovered_point_layer_idx;
                ctx.hover_debug_state->selected_parcel =
                    ctx.parcel_selection && !ctx.parcel_selection->active_entity_id.empty();
                ctx.hover_debug_state->selected_parcel_layer_idx =
                    ctx.parcel_selection ? ctx.parcel_selection->active_layer_idx : -1;
                ctx.hover_debug_state->selected_parcel_idx =
                    (ctx.parcel_selection && !ctx.parcel_selection->refs.empty() &&
                     ctx.parcel_selection->refs.back().feature_idx != (size_t)-1)
                        ? ctx.parcel_selection->refs.back().feature_idx
                    : (ctx.parcel_selection &&
                       ctx.parcel_selection->active_layer_idx >= 0 &&
                       (size_t)ctx.parcel_selection->active_layer_idx < ctx.layers->size())
                        ? featureIndexForGeometryEntityId(
                            (*ctx.layers)[(size_t)ctx.parcel_selection->active_layer_idx],
                            ctx.parcel_selection->refs.empty()
                                ? ctx.parcel_selection->active_entity_id
                                : ctx.parcel_selection->refs.back().geometry_entity_id)
                        : (size_t)-1;
                ctx.hover_debug_state->selected_parcel_entity_id =
                    ctx.parcel_selection ? ctx.parcel_selection->active_entity_id : std::string();
                ctx.hover_debug_state->selected_parcel_geometry_entity_id =
                    (ctx.parcel_selection && !ctx.parcel_selection->refs.empty())
                        ? ctx.parcel_selection->refs.back().geometry_entity_id
                        : std::string();
                ctx.hover_debug_state->selected_parcel_count =
                    ctx.parcel_selection ? ctx.parcel_selection->entity_ids.size() : 0;
            }

            MapFrameSessionContext map_frame_session_ctx;
            map_frame_session_ctx.root = ctx.root;
            map_frame_session_ctx.duckdb_analytics = ctx.duckdb_analytics;
            map_frame_session_ctx.draw = map_canvas_session.draw;
            map_frame_session_ctx.origin = map_canvas_session.origin;
            map_frame_session_ctx.size = map_canvas_session.size;
            map_frame_session_ctx.center_lon = ctx.center_lon;
            map_frame_session_ctx.center_lat = ctx.center_lat;
            map_frame_session_ctx.zoom_ptr = ctx.zoom;
            map_frame_session_ctx.max_zoom = ctx.max_zoom;
            map_frame_session_ctx.zoom = *ctx.zoom;
            map_frame_session_ctx.math_zoom = map_canvas_session.math_zoom;
            map_frame_session_ctx.zoom_scale = map_canvas_session.zoom_scale;
            map_frame_session_ctx.lod_ring_step = map_canvas_session.lod_ring_step;
            map_frame_session_ctx.real_property_layer_idx = ctx.real_property_layer_idx;
            map_frame_session_ctx.parcel_layer_idx = ctx.parcel_layer_idx;
            map_frame_session_ctx.zoning_layer_idx = ctx.zoning_layer_idx;
            map_frame_session_ctx.crime_nibrs_layer_idx = ctx.crime_nibrs_layer_idx;
            map_frame_session_ctx.vacant_notice_layer_idx = ctx.vacant_notice_layer_idx;
            map_frame_session_ctx.vacant_rehab_layer_idx = ctx.vacant_rehab_layer_idx;
            map_frame_session_ctx.tax_lien_layer_idx = ctx.tax_lien_layer_idx;
            map_frame_session_ctx.tax_sale_layer_idx = ctx.tax_sale_layer_idx;
            map_frame_session_ctx.vacant_notice_enabled = map_canvas_session.vacant_notice_enabled;
            map_frame_session_ctx.vacant_rehab_enabled = map_canvas_session.vacant_rehab_enabled;
            map_frame_session_ctx.tax_lien_enabled = map_canvas_session.tax_lien_enabled;
            map_frame_session_ctx.tax_sale_enabled = map_canvas_session.tax_sale_enabled;
            map_frame_session_ctx.view_min_lon = map_canvas_session.view_min_lon;
            map_frame_session_ctx.view_min_lat = map_canvas_session.view_min_lat;
            map_frame_session_ctx.view_max_lon = map_canvas_session.view_max_lon;
            map_frame_session_ctx.view_max_lat = map_canvas_session.view_max_lat;
            map_frame_session_ctx.global_heat_cell_px = ctx.global_heat_cell_px;
            map_frame_session_ctx.heatmap_algo = ctx.heatmap_algo;
            map_frame_session_ctx.heatmap_quality_preset = ctx.heatmap_quality_preset;
            map_frame_session_ctx.heatmap_bandwidth_px = ctx.heatmap_bandwidth_px;
            map_frame_session_ctx.heatmap_blur_sigma_px = ctx.heatmap_blur_sigma_px;
            map_frame_session_ctx.heatmap_percentile_clip = ctx.heatmap_percentile_clip;
            map_frame_session_ctx.heatmap_zoom_adaptive_bandwidth = ctx.heatmap_zoom_adaptive_bandwidth;
            map_frame_session_ctx.heatmap_multires_enabled = ctx.heatmap_multires_enabled;
            map_frame_session_ctx.heatmap_multires_blend = ctx.heatmap_multires_blend;
            map_frame_session_ctx.heatmap_controls_active = ctx.heatmap_controls_active;
            map_frame_session_ctx.parcel_parameter_mode = ctx.parcel_parameter_mode;
            map_frame_session_ctx.map_polygon_fill_opacity =
                ctx.app_settings ? ctx.app_settings->map_polygon_fill_opacity : 170.0f / 255.0f;
            map_frame_session_ctx.map_hovered = map_canvas_session.map_hovered;
            map_frame_session_ctx.parcel_hover_active = map_canvas_session.parcel_hover_active;
            map_frame_session_ctx.parcel_inspect_active = map_canvas_session.parcel_inspect_active;
            map_frame_session_ctx.zoning_hover_active = map_canvas_session.zoning_hover_active;
            map_frame_session_ctx.zoning_inspect_active = map_canvas_session.zoning_inspect_active;
            map_frame_session_ctx.vacancy_notice_color = &map_canvas_session.vacancy_notice_color;
            map_frame_session_ctx.vacancy_rehab_color = &map_canvas_session.vacancy_rehab_color;
            map_frame_session_ctx.layers = ctx.layers;
            map_frame_session_ctx.point_geometry_artifacts = ctx.point_geometry_artifacts;
            map_frame_session_ctx.polyline_geometry_artifacts = ctx.polyline_geometry_artifacts;
            map_frame_session_ctx.polygon_geometry_artifacts = ctx.polygon_geometry_artifacts;
            map_frame_session_ctx.parcel_render_blob = ctx.parcel_render_blob;
            map_frame_session_ctx.layer_spatial = ctx.layer_spatial;
            map_frame_session_ctx.map_filter_state = ctx.map_filter_state;
            map_frame_session_ctx.query_layers = ctx.query_layers;
            map_frame_session_ctx.real_property_by_blocklot = ctx.real_property_by_blocklot;
            map_frame_session_ctx.parcel_vac_notice_by_feature = ctx.parcel_vac_notice_by_feature;
            map_frame_session_ctx.parcel_vac_rehab_by_feature = ctx.parcel_vac_rehab_by_feature;
            map_frame_session_ctx.parcel_tax_lien_by_feature = ctx.parcel_tax_lien_by_feature;
            map_frame_session_ctx.parcel_tax_sale_by_feature = ctx.parcel_tax_sale_by_feature;
            map_frame_session_ctx.parcel_tax_lien_amount_by_feature = ctx.parcel_tax_lien_amount_by_feature;
            map_frame_session_ctx.parcel_tax_sale_amount_by_feature = ctx.parcel_tax_sale_amount_by_feature;
            map_frame_session_ctx.unified_parcels = ctx.unified_parcels;
            map_frame_session_ctx.parcel_owner_search_by_feature = ctx.parcel_owner_search_by_feature;
            map_frame_session_ctx.real_property_owner_search_by_feature = ctx.real_property_owner_search_by_feature;
            map_frame_session_ctx.parcel_address_search_by_feature = ctx.parcel_address_search_by_feature;
            map_frame_session_ctx.owner_text_filter_result_set = ctx.owner_text_filter_result_set;
            map_frame_session_ctx.address_text_filter_result_set = ctx.address_text_filter_result_set;
            map_frame_session_ctx.feature_render_cache = ctx.feature_render_cache;
            map_frame_session_ctx.selected_parcel_ids = ctx.selected_parcel_ids;
            map_frame_session_ctx.zoning_metadata = ctx.zoning_metadata;
            map_frame_session_ctx.zoning_zone_enabled = ctx.zoning_zone_enabled;
            map_frame_session_ctx.zoning_zone_color = ctx.zoning_zone_color;
            map_frame_session_ctx.parcel_selection = ctx.parcel_selection;
            map_frame_session_ctx.show_selected_zone_details = ctx.show_selected_zone_details;
            map_frame_session_ctx.selected_zone_idx = ctx.selected_zone_idx;
            map_frame_session_ctx.hover_state = &map_canvas_session.hover_state;
            map_frame_session_ctx.layer_fill_enabled = ctx.layer_fill_enabled;
            map_frame_session_ctx.layer_heatmap_enabled = ctx.layer_heatmap_enabled;
            map_frame_session_ctx.layer_heatmap_algo = ctx.layer_heatmap_algo;
            map_frame_session_ctx.layer_heatmap_max_zoom = ctx.layer_heatmap_max_zoom;
            map_frame_session_ctx.layer_parcel_detail_min_zoom = ctx.layer_parcel_detail_min_zoom;
            map_frame_session_ctx.layer_heatmap_cell_px = ctx.layer_heatmap_cell_px;
            map_frame_session_ctx.layer_heatmap_bandwidth_px = ctx.layer_heatmap_bandwidth_px;
            map_frame_session_ctx.layer_heatmap_blur_sigma_px = ctx.layer_heatmap_blur_sigma_px;
            map_frame_session_ctx.layer_heatmap_percentile_clip = ctx.layer_heatmap_percentile_clip;
            map_frame_session_ctx.layer_heatmap_zoom_adaptive_bandwidth = ctx.layer_heatmap_zoom_adaptive_bandwidth;
            map_frame_session_ctx.layer_heatmap_multires_enabled = ctx.layer_heatmap_multires_enabled;
            map_frame_session_ctx.layer_heatmap_multires_blend = ctx.layer_heatmap_multires_blend;
            map_frame_session_ctx.layer_heatmap_use_gradient = ctx.layer_heatmap_use_gradient;
            map_frame_session_ctx.layer_choropleth_gamma = ctx.layer_choropleth_gamma;
            map_frame_session_ctx.layer_normalize_mode = ctx.layer_normalize_mode;
            map_frame_session_ctx.parcel_jurisdiction_option_count = ctx.parcel_jurisdiction_option_count;
            map_frame_session_ctx.parcel_jurisdiction_filter_state = ctx.parcel_jurisdiction_filter_state;
            map_frame_session_ctx.heatmap_runtime = ctx.heatmap_runtime;
            map_frame_session_ctx.projection = map_canvas_session.projection_cache;
            map_frame_session_ctx.should_fill_layer_polygon = map_canvas_session.should_fill_layer_polygon;
            map_frame_session_ctx.project_world = map_canvas_session.project_world;
            map_frame_session_ctx.open_parcel_element = [&](const std::string& parcel_entity_id) {
                openElementParcelPage(*ctx.element_info_state, parcel_entity_id);
            };
            map_frame_session_ctx.prof_layer_ms_last = ctx.prof_layer_ms_last;
            map_frame_session_ctx.prof_owner_filter_ms_last = ctx.prof_owner_filter_ms_last;
            map_frame_session_ctx.prof_heatmap_ms_last = ctx.prof_heatmap_ms_last;
            map_frame_session_ctx.prof_owner_filter_candidates_last = ctx.prof_owner_filter_candidates_last;
            map_frame_session_ctx.prof_owner_filter_matches_last = ctx.prof_owner_filter_matches_last;
            map_frame_session_ctx.prof_heat_samples_last = ctx.prof_heat_samples_last;
            map_frame_session_ctx.prof_heatmap_gpu_splat_active = ctx.prof_heatmap_gpu_splat_active;
            map_frame_session_ctx.prof_heatmap_high_quality = ctx.prof_heatmap_high_quality;
            map_frame_session_ctx.prof_heatmap_cache_valid = ctx.prof_heatmap_cache_valid;
            map_frame_session_ctx.prof_heatmap_texture_resident = ctx.prof_heatmap_texture_resident;
            map_frame_session_ctx.prof_heatmap_async_inflight = ctx.prof_heatmap_async_inflight;
            map_frame_session_ctx.prof_heatmap_cache_key = ctx.prof_heatmap_cache_key;
            map_frame_session_ctx.prof_heatmap_texture_cache_entries = ctx.prof_heatmap_texture_cache_entries;
            map_frame_session_ctx.visible_vacant_parcels_last_frame = ctx.visible_vacant_parcels_last_frame;
            map_frame_session_ctx.prof_overlay_ms_last = ctx.prof_overlay_ms_last;
            map_frame_session_ctx.render_fill_attempts_last_frame = ctx.render_fill_attempts_last_frame;
            map_frame_session_ctx.render_fill_success_last_frame = ctx.render_fill_success_last_frame;
            map_frame_session_ctx.render_fill_no_triangles_last_frame = ctx.render_fill_no_triangles_last_frame;
            map_frame_session_ctx.render_fill_bad_indices_last_frame = ctx.render_fill_bad_indices_last_frame;
            map_frame_session_ctx.prof_projection_world_ring_cache_entries = ctx.prof_projection_world_ring_cache_entries;
            map_frame_session_ctx.prof_projection_world_extent_cache_entries = ctx.prof_projection_world_extent_cache_entries;
            map_frame_session_ctx.prof_projection_cache_generation = ctx.prof_projection_cache_generation;
            map_frame_session_ctx.prof_features_considered_frame = ctx.prof_features_considered_frame;
            map_frame_session_ctx.prof_features_drawn_frame = ctx.prof_features_drawn_frame;
    runMapFrameSession(map_frame_session_ctx);
    const bool road_label_click_consumed =
        !road_label_removal_consumed &&
        !roadLabelClickShouldDeferToPrimaryMapTarget(map_canvas_session) &&
        handleRoadLabelClickMode(ctx, map_canvas_session);
    if (road_label_click_consumed) {
        map_canvas_session.map_hovered = false;
    }
    drawMapCornerControlsVisual(ctx, map_canvas_session, map_corner_controls);
            std::string map_title = ctx.app_settings ? ctx.app_settings->map_title_text : std::string();
            if (ctx.app_settings && ctx.app_settings->map_title_all_caps) {
                map_title = toUpperAsciiCopy(map_title);
            }
    std::string title_source;
    if (!trimCopy(map_title).empty() && ctx.app_settings && ctx.app_settings->map_title_show_primary_parcel_source && ctx.layers) {
        const size_t source_layer_idx = resolveMapTitleSourceLayerIndex(
            *ctx.layers,
            ctx.app_settings->map_title_source_layer_file,
            ctx.parcel_layer_idx);
        if (source_layer_idx != (size_t)-1 && source_layer_idx < ctx.layers->size()) {
            title_source = mapTitleSourceLabelForLayer((*ctx.layers)[source_layer_idx]);
        }
    }
    drawMapTitleOverlay(map_canvas_session, map_title, title_source);
    if (ctx.app_settings && ctx.app_settings->map_legend_show_overlay) {
        drawMapLegendOverlay(
            map_canvas_session,
            ctx,
            std::clamp(ctx.app_settings->map_legend_overlay_position, 0, 3));
    }
    drawRoadLabelPopups(ctx.road_label_state, map_canvas_session, ctx.app_settings);
    if (ctx.target_indicator) {
        MapViewportFrame target_frame;
        target_frame.draw = map_canvas_session.draw;
        target_frame.origin = map_canvas_session.origin;
        target_frame.size = map_canvas_session.size;
        target_frame.math_zoom = map_canvas_session.math_zoom;
        target_frame.zoom_scale = map_canvas_session.zoom_scale;
        target_frame.center_world = map_canvas_session.center_world;
        drawMapTargetIndicator(*ctx.target_indicator, target_frame, ImGui::GetTime());
    }

    TimeCubePanelContext time_cube_panel_ctx;
    time_cube_panel_ctx.service = ctx.time_cube_service;
    time_cube_panel_ctx.layers = ctx.layers;
    time_cube_panel_ctx.result = ctx.time_cube_ui_result;
    time_cube_panel_ctx.loaded = ctx.time_cube_ui_loaded;
    time_cube_panel_ctx.status = ctx.time_cube_ui_status;
    time_cube_panel_ctx.mutex = ctx.time_cube_ui_mutex;
    time_cube_panel_ctx.worker = ctx.time_cube_ui_worker;
    time_cube_panel_ctx.running = ctx.time_cube_ui_running;
    time_cube_panel_ctx.done = ctx.time_cube_ui_done;
    time_cube_panel_ctx.selected = ctx.time_cube_selected;
    time_cube_panel_ctx.year_min = ctx.time_cube_year_min;
    time_cube_panel_ctx.year_max = ctx.time_cube_year_max;
    time_cube_panel_ctx.normalize_mode = ctx.time_cube_normalize_mode;
    time_cube_panel_ctx.show_excluded = ctx.time_cube_show_excluded;

    PolicyPanelContext policy_panel_ctx;
    policy_panel_ctx.hierarchy = ctx.policy_hierarchy;
    policy_panel_ctx.hierarchy_loaded = ctx.policy_hierarchy_loaded;
    policy_panel_ctx.hierarchy_error = ctx.policy_hierarchy_error;
    policy_panel_ctx.query = ctx.policy_hierarchy_query;
    policy_panel_ctx.query_capacity = ctx.policy_hierarchy_query_capacity;
    policy_panel_ctx.scope = ctx.policy_hierarchy_scope;
    policy_panel_ctx.roster = ctx.public_servant_roster;
    policy_panel_ctx.people_pay_cached_query = ctx.people_pay_cached_query;
    policy_panel_ctx.people_pay_cached_scope = ctx.people_pay_cached_scope;
    policy_panel_ctx.people_pay_cache_matched_count = ctx.people_pay_cache_matched_count;
    policy_panel_ctx.people_pay_visible_rows = ctx.people_pay_visible_rows;
    policy_panel_ctx.people_pay_cache_rebuilds = ctx.people_pay_cache_rebuilds;
    policy_panel_ctx.people_pay_rendered_rows_last = ctx.people_pay_rendered_rows_last;
    policy_panel_ctx.viz_root = ctx.policy_viz_root;
    policy_panel_ctx.viz_cached_query = ctx.policy_viz_cached_query;
    policy_panel_ctx.viz_cached_scope = ctx.policy_viz_cached_scope;
    policy_panel_ctx.viz_cached_metric = ctx.policy_viz_cached_metric;
    policy_panel_ctx.viz_metric = ctx.policy_viz_metric;
    policy_panel_ctx.viz_cache_rebuilds = ctx.policy_viz_cache_rebuilds;
    policy_panel_ctx.viz_node_count = ctx.policy_viz_node_count;
    drawMapOverlayPanelsPopup(map_canvas_session.origin, map_canvas_session.size, time_cube_panel_ctx, policy_panel_ctx, ctx.layers->size());
    ImGui::End();
}
