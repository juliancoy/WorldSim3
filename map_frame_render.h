#pragma once

#include "filters.h"
#include "heatmap_runtime.h"
#include "layer_runtime.h"
#include "map_render_projection.h"
#include "parcel_unified.h"
#include "render_policy.h"
#include "types.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct RenderFrameOrchestrationContext {
    const std::filesystem::path* root = nullptr;
    ImDrawList* draw = nullptr;
    ImVec2 origin = ImVec2(0.0f, 0.0f);
    ImVec2 size = ImVec2(0.0f, 0.0f);
    int zoom = 0;
    int math_zoom = 0;
    float zoom_scale = 1.0f;
    int lod_ring_step = 1;

    int zoning_layer_idx = -1;
    int parcel_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;

    bool vacant_notice_enabled = false;
    bool vacant_rehab_enabled = false;
    bool tax_lien_enabled = false;
    bool tax_sale_enabled = false;

    float view_min_lon = 0.0f;
    float view_min_lat = 0.0f;
    float view_max_lon = 0.0f;
    float view_max_lat = 0.0f;

    float global_heat_cell_px = 24.0f;
    int heatmap_algo = 0;
    int heatmap_quality_preset = 0;
    float heatmap_bandwidth_px = 18.0f;
    float heatmap_blur_sigma_px = 6.0f;
    float heatmap_percentile_clip = 95.0f;
    bool heatmap_zoom_adaptive_bandwidth = true;
    bool heatmap_multires_enabled = true;
    float heatmap_multires_blend = 0.5f;
    bool heatmap_allow_cpu_fallback = false;
    bool heatmap_controls_active = false;

    int parcel_parameter_mode = 0;

    const ImVec4* vacancy_notice_color = nullptr;
    const ImVec4* vacancy_rehab_color = nullptr;

    std::vector<LayerDef>* layers = nullptr;
    std::vector<LayerSpatialIndex>* layer_spatial = nullptr;
    std::vector<size_t>* layer_fallback_scan_cursor = nullptr;
    const std::vector<bool>* layer_fill_enabled = nullptr;
    const std::vector<bool>* layer_heatmap_enabled = nullptr;
    const std::vector<int>* layer_heatmap_algo = nullptr;
    const std::vector<int>* layer_heatmap_max_zoom = nullptr;
    const std::vector<int>* layer_parcel_detail_min_zoom = nullptr;
    const std::vector<float>* layer_heatmap_cell_px = nullptr;
    const std::vector<float>* layer_heatmap_bandwidth_px = nullptr;
    const std::vector<float>* layer_heatmap_blur_sigma_px = nullptr;
    const std::vector<float>* layer_heatmap_percentile_clip = nullptr;
    const std::vector<bool>* layer_heatmap_zoom_adaptive_bandwidth = nullptr;
    const std::vector<bool>* layer_heatmap_multires_enabled = nullptr;
    const std::vector<float>* layer_heatmap_multires_blend = nullptr;
    const std::vector<bool>* layer_heatmap_use_gradient = nullptr;
    const std::vector<float>* layer_choropleth_gamma = nullptr;
    const std::vector<int>* layer_normalize_mode = nullptr;
    const std::vector<int>* parcel_vac_notice_by_feature = nullptr;
    const std::vector<int>* parcel_vac_rehab_by_feature = nullptr;
    const std::vector<int>* parcel_tax_lien_by_feature = nullptr;
    const std::vector<int>* parcel_tax_sale_by_feature = nullptr;
    const std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;
    const std::vector<size_t>* selected_parcel_indices = nullptr;
    const std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr;
    const std::unordered_map<std::string, ImVec4>* zoning_zone_color = nullptr;
    const std::vector<QueryMapLayer>* query_layers = nullptr;
    const std::unordered_set<std::string>* selected_owners = nullptr;

    HeatmapRuntimeState* heatmap_runtime = nullptr;
    MapProjectionCache* projection = nullptr;

    std::function<bool(size_t)> is_parcel_related_layer;
    std::function<bool(size_t)> layer_uses_heatmap_aggregate;
    std::function<bool(size_t)> layer_uses_lod_geometry;
    std::function<bool(size_t, size_t, const LayerDef::FeatureGeom&)> feature_passes_filters;
    std::function<bool(size_t, size_t, const LayerDef::FeatureGeom&, ImU32&)> query_map_color;
    std::function<bool(size_t)> should_fill_layer_polygon;
    std::function<ImVec2(const ImVec2&)> project_world;

    std::chrono::steady_clock::time_point layer_prof_begin;

    std::atomic<double>* prof_layer_ms_last = nullptr;
    std::atomic<double>* prof_heatmap_ms_last = nullptr;
    std::atomic<size_t>* prof_heat_samples_last = nullptr;
    std::atomic<bool>* prof_heatmap_gpu_splat_active = nullptr;
    std::atomic<bool>* prof_heatmap_high_quality = nullptr;
    std::atomic<bool>* prof_heatmap_cache_valid = nullptr;
    std::atomic<bool>* prof_heatmap_texture_resident = nullptr;
    std::atomic<bool>* prof_heatmap_async_inflight = nullptr;
    std::atomic<uint64_t>* prof_heatmap_cache_key = nullptr;
    std::atomic<size_t>* prof_heatmap_texture_cache_entries = nullptr;
    std::atomic<size_t>* visible_vacant_parcels_last_frame = nullptr;
    std::atomic<double>* prof_overlay_ms_last = nullptr;
    std::atomic<size_t>* render_fill_attempts_last_frame = nullptr;
    std::atomic<size_t>* render_fill_success_last_frame = nullptr;
    std::atomic<size_t>* render_fill_no_triangles_last_frame = nullptr;
    std::atomic<size_t>* render_fill_bad_indices_last_frame = nullptr;

    size_t* prof_features_considered_frame = nullptr;
    size_t* prof_features_drawn_frame = nullptr;

    const char* filter_blocklot = "";
    const char* filter_status = "";
    const char* filter_address = "";
    const char* filter_owner = "";
    const char* filter_zip = "";
    bool filter_enabled = false;
    bool filter_use_date = false;
    int filter_year_min = 0;
    int filter_year_max = 0;

    bool crime_filter_enabled = false;
    bool crime_filter_use_year = false;
    int crime_year_min = 0;
    int crime_year_max = 0;
    bool crime_filter_homicide = false;
    bool crime_filter_robbery = false;
    bool crime_filter_assault = false;
    bool crime_filter_burglary = false;
    bool crime_filter_theft = false;
    bool crime_filter_auto_theft = false;
    bool crime_filter_drug = false;
    bool crime_filter_shooting = false;
};

void orchestrateMapFrameRender(const RenderFrameOrchestrationContext& ctx);
