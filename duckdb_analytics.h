#pragma once

#include "types.h"
#include "filters.h"
#include "parcel_unified.h"

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct DuckDbAnalyticsStatus {
    bool available = false;
    bool last_rebuild_ok = false;
    size_t layer_count = 0;
    size_t feature_count = 0;
    std::string db_path;
    std::string message;
};

struct DuckDbArtifactEnsureResult {
    bool ok = false;
    bool rebuilt = false;
    bool incrementally_updated = false;
    bool reused_existing = false;
    bool invalidated = false;
    std::string message;
};

struct DuckDbQueryResult {
    bool ok = false;
    std::string message;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    FilterResultSet result_set;
};

struct DuckDbSelectedParcel {
    size_t layer_idx = 0;
    std::string entity_id;
    std::string blocklot;
};

struct DuckDbSearchHit {
    size_t layer_idx = 0;
    std::string entity_id;
    std::string blocklot;
    std::string owner;
    std::string address;
    double current_value = 0.0;
    int score = 0;
};

struct DuckDbParcelSemanticSnapshot {
    bool ok = false;
    std::string source_signature;
    std::vector<UnifiedParcelRecord> unified_parcels;
    std::vector<std::string> parcel_blocklot_by_feature;
    std::vector<std::string> parcel_owner_search_by_feature;
    std::vector<std::string> parcel_address_search_by_feature;
    std::vector<int> parcel_vac_notice_by_feature;
    std::vector<int> parcel_vac_rehab_by_feature;
    std::vector<int> parcel_tax_lien_by_feature;
    std::vector<int> parcel_tax_sale_by_feature;
    std::vector<double> parcel_tax_lien_amount_by_feature;
    std::vector<double> parcel_tax_sale_amount_by_feature;
    std::string message;
};

class DuckDbAnalytics {
public:
    explicit DuckDbAnalytics(std::filesystem::path root);

    const DuckDbAnalyticsStatus& status() const { return status_; }
    bool needsRebuild(const std::vector<LayerDef>& layers) const;
    bool validateExistingCache();
    std::string buildSourceSignature() const;
    DuckDbArtifactEnsureResult ensureCurrentArtifact(
        const std::vector<LayerDef>& layers,
        const std::vector<UnifiedParcelRecord>& unified_parcels = {});
    bool rebuild(const std::vector<LayerDef>& layers, const std::vector<UnifiedParcelRecord>& unified_parcels = {});
    DuckDbQueryResult executeMapQuery(
        const std::string& sql,
        const std::unordered_set<std::string>& selected_owners,
        const std::vector<DuckDbSelectedParcel>& selected_parcels,
        size_t max_rows = 1000) const;
    DuckDbQueryResult queryParcelJurisdictions(
        size_t parcel_layer_idx,
        const std::unordered_set<std::string>& jurisdictions,
        size_t max_rows = 1000) const;
    DuckDbQueryResult queryUnifiedParcelDetail(const std::string& parcel_entity_id) const;
    DuckDbQueryResult queryParcelEvents(
        const std::string& blocklot,
        size_t max_rows = 200) const;
    std::vector<DuckDbSearchHit> searchParcels(const std::string& query, size_t max_rows = 100) const;
    DuckDbParcelSemanticSnapshot loadParcelSemanticSnapshot(size_t parcel_layer_idx) const;

private:
    std::filesystem::path root_;
    DuckDbAnalyticsStatus status_;
    mutable std::mutex parcel_detail_cache_mutex_;
    mutable std::unordered_map<std::string, DuckDbQueryResult> parcel_detail_cache_;
};
