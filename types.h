#pragma once

#include "imgui.h"

#include <string>
#include <utility>
#include <vector>
#include <cstdint>

struct LayerDef {
    enum class Category {
        Housing,
        PublicHealth,
        Infrastructure,
        Zoning,
        Safety
    };
    std::string name;
    std::string file;
    std::string source_url;
    std::string reference_url;
    std::vector<std::string> source_urls;
    std::string import_type;
    std::string import_url;
    std::string import_source_crs;
    std::string import_shapefile;
    std::string import_service_url;
    std::string import_normalizer;
    std::string import_sheet_name;
    std::string import_lon_field;
    std::string import_lat_field;
    std::string import_artifact_file;
    std::string duckdb_role;
    std::string provenance_world;
    std::string provenance_nation_state;
    std::string provenance_state_region;
    std::string provenance_county_city;
    std::string description;
    std::string heatmap_field;
    std::string subcategory;
    std::string region;
    std::string scale;
    ImVec4 color;
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
    struct FeatureGeom {
        FeatureExtent extent;
        std::vector<std::vector<ImVec2>> rings;
        std::vector<uint32_t> triangles;
        std::vector<std::pair<std::string, std::string>> properties;
    };
    std::vector<FeatureGeom> features;
};
