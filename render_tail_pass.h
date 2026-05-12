#pragma once

#include "imgui.h"
#include "map_render_projection.h"
#include "parcel_unified.h"
#include "types.h"

#include <cstddef>
#include <functional>
#include <vector>

struct RenderTailPassContext {
    ImDrawList* draw = nullptr;
    ImVec2 origin = ImVec2(0.0f, 0.0f);
    ImVec2 size = ImVec2(0.0f, 0.0f);
    int zoom = 0;
    int math_zoom = 0;
    int parcel_layer_idx = -1;
    int parcel_parameter_mode = 0;
    float parcel_choropleth_gamma = 1.0f;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
    bool vacant_notice_overlay_enabled = false;
    bool vacant_rehab_overlay_enabled = false;
    bool tax_lien_overlay_enabled = false;
    bool tax_sale_overlay_enabled = false;
    const ImVec4* vacancy_notice_color = nullptr;
    const ImVec4* vacancy_rehab_color = nullptr;
    std::vector<LayerDef>* layers = nullptr;
    const std::vector<bool>* layer_fill_enabled = nullptr;
    const std::vector<int>* parcel_vac_notice_by_feature = nullptr;
    const std::vector<int>* parcel_vac_rehab_by_feature = nullptr;
    const std::vector<int>* parcel_tax_lien_by_feature = nullptr;
    const std::vector<int>* parcel_tax_sale_by_feature = nullptr;
    const std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;
    const std::vector<size_t>* selected_parcel_indices = nullptr;
    MapProjectionCache* projection = nullptr;
    std::function<bool(size_t, size_t, const LayerDef::FeatureGeom&)> feature_passes_filters;
    std::function<bool(size_t)> should_fill_layer_polygon;
    std::function<ImVec2(const ImVec2&)> project_world;
};

struct RenderTailPassResult {
    size_t visible_vacant_parcels = 0;
    MapFillStats fill_stats;
};

RenderTailPassResult runRenderTailPass(const RenderTailPassContext& ctx);
