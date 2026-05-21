#pragma once

#include "cache_io.h"
#include "layer_runtime.h"
#include "map_render_hover.h"
#include "parcel_unified.h"
#include "selection.h"
#include "types.h"
#include "zoning.h"

#include <cstddef>
#include <functional>
#include <unordered_map>
#include <vector>

struct MapInspectionContext {
    bool map_hovered = false;
    bool parcel_hover_active = false;
    bool parcel_inspect_active = false;
    bool zoning_hover_active = false;
    bool zoning_inspect_active = false;
    int parcel_layer_idx = -1;
    int zoning_layer_idx = -1;
    const std::vector<LayerDef>* layers = nullptr;
    const std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;
    const std::unordered_map<size_t, PolygonGeometryArtifact>* polygon_geometry_artifacts = nullptr;
    std::vector<LayerSpatialIndex>* layer_spatial = nullptr;
    const std::unordered_map<std::string, ZoneMetadata>* zoning_metadata = nullptr;
    ParcelSelectionState* parcel_selection = nullptr;
    std::function<void(size_t)> open_parcel_element;
    bool* show_selected_zone_details = nullptr;
    size_t* selected_zone_idx = nullptr;
    const MapHoverState* hover_state = nullptr;
    const std::vector<int>* parcel_vac_notice_by_feature = nullptr;
    const std::vector<int>* parcel_vac_rehab_by_feature = nullptr;
    const std::vector<int>* parcel_tax_lien_by_feature = nullptr;
    const std::vector<int>* parcel_tax_sale_by_feature = nullptr;
    const std::vector<double>* parcel_tax_lien_amount_by_feature = nullptr;
    const std::vector<double>* parcel_tax_sale_amount_by_feature = nullptr;
    const std::unordered_map<std::string, size_t>* real_property_by_blocklot = nullptr;
    int real_property_layer_idx = -1;
};

void handleMapInspection(const MapInspectionContext& ctx);
