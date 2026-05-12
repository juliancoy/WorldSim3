#pragma once

#include "types.h"
#include "filters.h"
#include "parcel_unified.h"

#include <cstddef>
#include <filesystem>
#include <string>
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

struct DuckDbQueryResult {
    bool ok = false;
    std::string message;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    FilterResultSet result_set;
};

struct DuckDbSelectedParcel {
    size_t layer_idx = 0;
    size_t feature_idx = 0;
    std::string blocklot;
};

struct DuckDbSearchHit {
    size_t layer_idx = 0;
    size_t feature_idx = 0;
    std::string blocklot;
    std::string owner;
    std::string address;
    double current_value = 0.0;
    int score = 0;
};

class DuckDbAnalytics {
public:
    explicit DuckDbAnalytics(std::filesystem::path root);

    const DuckDbAnalyticsStatus& status() const { return status_; }
    bool needsRebuild(const std::vector<LayerDef>& layers) const;
    bool validateExistingCache();
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
    std::vector<DuckDbSearchHit> searchParcels(const std::string& query, size_t max_rows = 100) const;

private:
    std::filesystem::path root_;
    DuckDbAnalyticsStatus status_;
};
