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
    std::string description;
    std::string heatmap_field;
    std::string subcategory;
    std::string scale;
    ImVec4 color;
    bool enabled = false;
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
