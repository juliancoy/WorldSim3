#pragma once

#include "heat_normalization.h"
#include "heatmap_render.h"
#include "imgui.h"
#include "layer_runtime.h"
#include "map_render_projection.h"
#include "render_plan_builder.h"
#include "render_policy.h"
#include "types.h"
#include "worldsim_app_internal.h"

#include <functional>
#include <unordered_map>
#include <vector>

struct RenderLayerPassContext {
    ImDrawList* draw = nullptr;
    ImVec2 origin = ImVec2(0.0f, 0.0f);
    ImVec2 size = ImVec2(0.0f, 0.0f);
    int math_zoom = 0;
    float zoom_scale = 1.0f;
    int lod_ring_step = 1;
    int parcel_layer_idx = -1;
    int zoning_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    const ImVec4* vacancy_notice_color = nullptr;
    const ImVec4* vacancy_rehab_color = nullptr;
    bool vacant_notice_overlay_enabled = false;
    bool vacant_rehab_overlay_enabled = false;
    float view_min_lon = 0.0f;
    float view_min_lat = 0.0f;
    float view_max_lon = 0.0f;
    float view_max_lat = 0.0f;
    bool should_recompute_heatmap = false;
    bool high_quality_gpu_aggregate = false;
    bool smooth_only_heatmap = false;
    bool can_use_cached_heatmap = false;
    const std::vector<LayerDef>* layers = nullptr;
    std::vector<LayerSpatialIndex>* layer_spatial = nullptr;
    const std::vector<bool>* layer_fill_enabled = nullptr;
    const std::vector<bool>* layer_heatmap_use_gradient = nullptr;
    const std::vector<float>* layer_choropleth_gamma = nullptr;
    const std::vector<int>* layer_normalize_mode = nullptr;
    const std::vector<float>* layer_heatmap_percentile_clip = nullptr;
    const std::vector<int>* parcel_vac_notice_by_feature = nullptr;
    const std::vector<int>* parcel_vac_rehab_by_feature = nullptr;
    const std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr;
    const std::unordered_map<std::string, ImVec4>* zoning_zone_color = nullptr;
    const HeatmapLayerPolicyContext* heatmap_policy = nullptr;
    const RenderPlan* render_plan = nullptr;
    RawSourceLayerPolicy raw_source_layer_policy;
    std::vector<HeatSample>* heat_samples = nullptr;
    MapProjectionCache* projection = nullptr;
    std::function<bool(size_t, size_t, const LayerDef::FeatureGeom&)> feature_passes_filters;
    std::function<bool(size_t, size_t, const LayerDef::FeatureGeom&, ImU32&)> query_map_color;
    std::function<bool(size_t)> should_fill_layer_polygon;
    std::function<ImVec2(const ImVec2&)> project_world;
    size_t* prof_features_considered_frame = nullptr;
    size_t* prof_features_drawn_frame = nullptr;
};

void runRenderLayerPass(const RenderLayerPassContext& ctx);
