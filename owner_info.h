#pragma once

#include "duckdb_analytics.h"
#include "parcel_unified.h"
#include "types.h"

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

enum class ElementInfoKind {
    None,
    Parcel,
    Owner
};

struct ElementInfoEntry {
    ElementInfoKind kind = ElementInfoKind::None;
    size_t parcel_idx = (size_t)-1;
    std::string owner;
};

struct ElementInfoUiState {
    std::vector<ElementInfoEntry> history;
    size_t history_index = (size_t)-1;
    bool tab_requested = false;
    char* property_query = nullptr;
    size_t property_query_size = 0;
};

using OwnerInfoUiState = ElementInfoUiState;

struct OwnerInfoTabContext {
    ElementInfoUiState* state = nullptr;
    DuckDbAnalytics* duckdb_analytics = nullptr;
    const std::vector<LayerDef>* layers = nullptr;
    int parcel_layer_idx = -1;
    const std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;
    const std::unordered_set<size_t>* selected_parcel_index_set = nullptr;
    const std::vector<size_t>* selected_parcel_indices = nullptr;
    bool show_selected_parcel_details = false;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
    double* center_lon = nullptr;
    double* center_lat = nullptr;
    int* zoom = nullptr;
    std::function<void()> clear_parcel_selection;
    std::function<bool(size_t, bool)> select_parcel_idx;
    std::function<const LayerDef::FeatureGeom*(const LayerDef::FeatureGeom&)> real_property_for_parcel;
};

void openElementParcelPage(ElementInfoUiState& state, size_t parcel_idx);
void openOwnerInfoPage(ElementInfoUiState& state, const std::string& owner);
void drawOwnerInfoLink(ElementInfoUiState& state, const std::string& owner, const char* id);
void drawElementInfoTab(const OwnerInfoTabContext& ctx);
