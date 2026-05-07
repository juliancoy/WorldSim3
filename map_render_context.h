#pragma once

#include "imgui.h"
#include "map_render_projection.h"
#include "types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

struct MapRenderContext {
    ImDrawList* draw = nullptr;
    ImVec2 origin;
    ImVec2 size;
    std::vector<LayerDef>* layers = nullptr;
    size_t parcel_layer_idx = (size_t)-1;

    bool vacant_notice_enabled = false;
    bool vacant_rehab_enabled = false;
    bool tax_lien_enabled = false;
    bool tax_sale_enabled = false;

    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;

    ImVec4 vacancy_notice_color = ImVec4(1, 0, 0, 1);
    ImVec4 vacancy_rehab_color = ImVec4(0, 1, 1, 1);

    const std::vector<bool>* layer_fill_enabled = nullptr;
    const std::vector<int>* parcel_vac_notice_by_feature = nullptr;
    const std::vector<int>* parcel_vac_rehab_by_feature = nullptr;
    const std::vector<int>* parcel_tax_lien_by_feature = nullptr;
    const std::vector<int>* parcel_tax_sale_by_feature = nullptr;

    std::function<bool(size_t, size_t, const LayerDef::FeatureGeom&)> feature_passes_filters;
    std::function<bool(size_t)> should_fill_layer_polygon;
    MapProjectionCache* projection = nullptr;
};

struct MapOverlayResult {
    size_t visible_vacant_parcels = 0;
};
