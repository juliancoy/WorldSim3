#pragma once

#include "cache_io.h"
#include "duckdb_analytics.h"
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
    DuckDbAnalytics* duckdb_analytics = nullptr;
    const ParcelRenderCacheBlob* parcel_render_blob = nullptr;
    const std::unordered_map<size_t, PolygonGeometryArtifact>* polygon_geometry_artifacts = nullptr;
    std::vector<LayerSpatialIndex>* layer_spatial = nullptr;
    const std::unordered_map<std::string, ZoneMetadata>* zoning_metadata = nullptr;
    ParcelSelectionState* parcel_selection = nullptr;
    std::function<void(const std::string&)> open_parcel_element;
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

struct ParcelHoverResolution {
    bool hit = false;
    int layer_idx = -1;
    size_t feature_idx = (size_t)-1;
    std::string entity_id;
    std::string geometry_entity_id;
    const UnifiedParcelRecord* unified_record = nullptr;
    bool has_duckdb_record = false;
    UnifiedParcelRecord duckdb_record;
};

struct ParcelHoverDetail {
    bool available = false;
    std::string parcel_entity_id;
    std::string blocklot;
    std::string owner;
    std::string owner_display;
    std::string address;
    std::string zipcode;
    std::string status;
    std::string property_source_file;
    bool parcel_has_geometry = false;
    bool has_property_record = false;
    LayerDef::FeatureExtent parcel_extent;
    double current_land = 0.0;
    double current_improvements = 0.0;
    double structure_area_sqft = 0.0;
    double tax_base = 0.0;
    double sale_price = 0.0;
    double current_value = 0.0;
    int vacant_notice_count = 0;
    int vacant_rehab_count = 0;
    int tax_lien_count = 0;
    int tax_sale_count = 0;
    double tax_lien_amount = 0.0;
    double tax_sale_amount = 0.0;
};

ParcelHoverResolution resolveHoveredParcel(const MapInspectionContext& ctx);
ParcelHoverResolution resolveInspectParcel(const MapInspectionContext& ctx);
ParcelHoverDetail resolveParcelHoverDetail(const MapInspectionContext& ctx, const ParcelHoverResolution& hovered);
bool applyParcelClickSelection(const MapInspectionContext& ctx, const ParcelHoverResolution& hovered, bool ctrl_append);
void handleMapInspection(const MapInspectionContext& ctx);
