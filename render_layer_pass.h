#pragma once

#include "heat_normalization.h"
#include "heatmap_runtime.h"
#include "heatmap_render.h"
#include "imgui.h"
#include "layer_runtime.h"
#include "filters.h"
#include "cache_io.h"
#include "map_render_hover.h"
#include "map_render_projection.h"
#include "render_plan_builder.h"
#include "render_policy.h"
#include "types.h"
#include "worldsim_app_internal.h"

#include <functional>
#include <filesystem>
#include <unordered_map>
#include <vector>

struct RenderLayerPassContext {
    const std::filesystem::path* root = nullptr;
    ImDrawList* draw = nullptr;
    ImVec2 origin = ImVec2(0.0f, 0.0f);
    ImVec2 size = ImVec2(0.0f, 0.0f);
    double* center_lon = nullptr;
    double* center_lat = nullptr;
    double* zoom = nullptr;
    int max_zoom = 0;
    int math_zoom = 0;
    double zoom_value = 0.0;
    float zoom_scale = 1.0f;
    int lod_ring_step = 1;
    int parcel_layer_idx = -1;
    int zoning_layer_idx = -1;
    int crime_nibrs_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    float map_polygon_fill_opacity = 170.0f / 255.0f;
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
    uint64_t heatmap_data_key = 0;
    const std::vector<LayerDef>* layers = nullptr;
    const std::unordered_map<size_t, PointGeometryArtifact>* point_geometry_artifacts = nullptr;
    const std::unordered_map<size_t, PolylineGeometryArtifact>* polyline_geometry_artifacts = nullptr;
    const std::unordered_map<size_t, PolygonGeometryArtifact>* polygon_geometry_artifacts = nullptr;
    const ParcelRenderCacheBlob* parcel_render_blob = nullptr;
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
    HeatmapRuntimeState* heatmap_runtime = nullptr;
    const RenderPlan* render_plan = nullptr;
    const std::vector<QueryMapLayer>* query_layers = nullptr;
    RawSourceLayerPolicy raw_source_layer_policy;
    std::vector<HeatSample>* heat_samples = nullptr;
    MapProjectionCache* projection = nullptr;
    const MapHoverState* hover_state = nullptr;
    std::function<bool(size_t)> layer_passes_filters;
    std::function<bool(size_t, size_t, const LayerDef::FeatureRecord&)> feature_passes_filters;
    std::function<bool(size_t, size_t, const LayerDef::FeatureRecord&, ImU32&)> query_map_color;
    std::function<bool(size_t)> should_fill_layer_polygon;
    std::function<ImVec2(const ImVec2&)> project_world;
    const char* filter_blocklot = "";
    const char* filter_status = "";
    const char* filter_address = "";
    const char* filter_owner = "";
    const char* filter_zip = "";
    bool filter_enabled = false;
    size_t* prof_features_considered_frame = nullptr;
    size_t* prof_features_drawn_frame = nullptr;
};

void runRenderLayerPass(const RenderLayerPassContext& ctx);

bool aggregateSampleAnchorLonLatForFeature(
    const RenderLayerPassContext& ctx,
    size_t layer_idx,
    size_t feature_idx,
    const LayerDef::FeatureRecord& fg,
    float& out_lon,
    float& out_lat);

bool shouldBypassCpuParcelFeaturePass(
    bool parcel_gpu_draw_active,
    bool layer_uses_heatmap_for_cache,
    bool layer_uses_lod_for_draw,
    bool should_recompute_heatmap);

bool shouldUseCrimePointPrimaryGpuDraw(
    bool crime_gpu_draw_active,
    bool layer_uses_heatmap_for_cache,
    bool layer_uses_lod_for_draw,
    bool layer_uses_point_clustering);
