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
    Owner,
    ParcelSource,
    PropertySource
};

struct ElementInfoEntry {
    ElementInfoKind kind = ElementInfoKind::None;
    std::string parcel_entity_id;
    std::string owner;
    std::string source;
};

struct OwnerSimilarMatch {
    std::string owner;
    size_t property_count = 0;
    double current_value = 0.0;
    int score = 0;
};

struct ElementInfoUiState {
    std::vector<ElementInfoEntry> history;
    size_t history_index = (size_t)-1;
    bool tab_requested = false;
    char* property_query = nullptr;
    size_t property_query_size = 0;
    int owner_fuzzy_match_min_score = 100;
    int owner_fuzzy_match_limit = 12;
    std::string owner_fuzzy_cache_key;
    int owner_fuzzy_cache_min_score = 100;
    int owner_fuzzy_cache_limit = 12;
    std::vector<OwnerSimilarMatch> owner_fuzzy_cache_matches;
    std::string owner_duckdb_cache_key;
    std::vector<UnifiedParcelRecord> owner_duckdb_cache_records;
};

using OwnerInfoUiState = ElementInfoUiState;

struct OwnerInfoTabContext {
    ElementInfoUiState* state = nullptr;
    DuckDbAnalytics* duckdb_analytics = nullptr;
    const std::vector<LayerDef>* layers = nullptr;
    int parcel_layer_idx = -1;
    int real_property_layer_idx = -1;
    const std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;
    const std::unordered_map<std::string, size_t>* real_property_by_blocklot = nullptr;
    const std::unordered_set<std::string>* selected_parcel_id_set = nullptr;
    const std::vector<std::string>* selected_parcel_ids = nullptr;
    bool show_selected_parcel_details = false;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
    double* center_lon = nullptr;
    double* center_lat = nullptr;
    double* zoom = nullptr;
    int min_zoom = 0;
    int max_zoom = 0;
    float map_view_w = 0.0f;
    float map_view_h = 0.0f;
    std::function<void()> clear_parcel_selection;
    std::function<bool(const std::string&, bool)> select_parcel_id;
};

void openElementParcelPage(ElementInfoUiState& state, const std::string& parcel_entity_id);
void openOwnerInfoPage(ElementInfoUiState& state, const std::string& owner);
void openOwnerInfoPageAndSelectOwnerParcels(
    ElementInfoUiState& state,
    const std::string& owner,
    const std::vector<UnifiedParcelRecord>* unified_parcels,
    const std::function<void()>& clear_parcel_selection,
    const std::function<bool(const std::string&, bool)>& select_parcel_id);
void openParcelSourceInfoPage(ElementInfoUiState& state, const std::string& source);
void openPropertySourceInfoPage(ElementInfoUiState& state, const std::string& source);
void drawOwnerInfoLink(ElementInfoUiState& state, const std::string& owner, const char* id);
void drawSourceInfoLink(ElementInfoUiState& state, const char* label, const std::string& source, bool property_source, const char* id);
void drawElementInfoTab(const OwnerInfoTabContext& ctx);
