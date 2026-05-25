#pragma once

#include "imgui.h"

#include <string>
#include <utility>
#include <vector>
#include <cstdint>

enum class GeometryArtifactClass {
    Unknown,
    Point,
    Polyline,
    Polygon
};

struct LayerDef {
    enum class Category {
        Housing,
        PublicHealth,
        Infrastructure,
        Zoning,
        Safety
    };
    std::string name;
    std::string logical_id;
    std::string file;
    std::string source_url;
    std::string reference_url;
    std::vector<std::string> source_urls;
    std::string import_type;
    std::string import_url;
    std::string import_source_crs;
    std::string import_shapefile;
    std::string import_service_url;
    std::string import_where;
    std::string import_normalizer;
    std::string import_query;
    std::string import_table;
    std::string import_year;
    std::string import_survey;
    std::string import_sheet_name;
    std::string import_item_path;
    std::string import_lon_field;
    std::string import_lat_field;
    std::string import_artifact_file;
    std::string duckdb_role;
    std::string provenance_world;
    std::string provenance_nation_state;
    std::string provenance_state_region;
    std::string provenance_county_city;
    mutable bool normalized_geography_cache_valid = false;
    mutable bool geometry_usage_cache_valid = false;
    mutable bool render_classification_cache_valid = false;
    mutable bool uses_point_geometry_cache = false;
    mutable bool uses_polyline_geometry_cache = false;
    mutable bool zoning_polygon_layer_cache = false;
    mutable uint8_t point_marker_glyph_cache = 0;
    mutable std::string normalized_provenance_nation_state;
    mutable std::string normalized_provenance_state_region;
    mutable std::string normalized_provenance_county_city;
    std::string description;
    std::string heatmap_field;
    std::string subcategory;
    std::string region;
    std::string scale;
    ImVec4 color;
    ImVec4 outline_color;
    bool enabled = false;
    bool runtime_load = true;
    bool duckdb_ingest = true;
    Category category = Category::Housing;
    struct FeatureExtent {
        float min_lon = 0.0f;
        float min_lat = 0.0f;
        float max_lon = 0.0f;
        float max_lat = 0.0f;
    };
    struct FeatureRecord {
        std::string entity_id;
        std::string geometry_entity_id;
        std::string source_feature_id;
        std::string source_primary_key;
        FeatureExtent extent;
        std::vector<std::vector<ImVec2>> rings;
        std::vector<std::vector<ImVec2>> paths;
        std::vector<uint32_t> triangles;
    };
    struct FeatureProperties {
        std::vector<std::pair<std::string, std::string>> values;
    };
    std::vector<FeatureRecord> features;
    std::vector<FeatureProperties> feature_properties;
};
