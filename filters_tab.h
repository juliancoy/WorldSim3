#pragma once

#include "filters.h"
#include "types.h"
#include "zoning.h"

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct AddressLocateMatch {
    size_t parcel_idx = (size_t)-1;
    int score = 0;
    std::string address;
};

struct FiltersTabContext {
    MapFilterState* filters = nullptr;
    const std::vector<LayerDef>* layers = nullptr;
    const std::unordered_map<std::string, ZoneMetadata>* zoning_metadata = nullptr;
    const std::unordered_set<size_t>* selected_parcel_index_set = nullptr;
    int parcel_layer_idx = -1;
    int zoning_layer_idx = -1;
    bool* show_selected_parcel_details = nullptr;
    bool* show_selected_zone_details = nullptr;
    size_t* selected_zone_idx = nullptr;
    double* center_lon = nullptr;
    double* center_lat = nullptr;
    int* zoom = nullptr;
    std::string* address_locate_status = nullptr;
    std::vector<AddressLocateMatch>* address_locate_matches = nullptr;
    std::vector<int>* record_year_hist = nullptr;
    std::vector<float>* record_year_hist_plot = nullptr;
    std::vector<size_t>* hist_feature_counts = nullptr;
    std::vector<bool>* hist_enabled = nullptr;
    bool* hist_dirty = nullptr;
    float* record_year_hist_max_bin = nullptr;
    int* record_year_nonzero_min = nullptr;
    int* record_year_nonzero_max = nullptr;
    int* record_year_nonzero_total = nullptr;
    int* selected_record_year = nullptr;
    bool* selected_record_year_dirty = nullptr;
    int* selected_record_year_total = nullptr;
    std::vector<std::string>* selected_record_year_samples = nullptr;
    std::function<void()> clear_parcel_selection;
    std::function<bool(size_t, bool)> select_parcel_idx;
    std::function<const LayerDef::FeatureGeom*(const LayerDef::FeatureGeom&)> real_property_for_parcel;
};

void drawFiltersTab(const FiltersTabContext& ctx);
