#pragma once

#include "types.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

struct TimeCubeQuery {
    std::string layer_filter;
    int year_from = 1900;
    int year_to = 2100;
    bool enabled_only = false;
    bool rebuild = false;
};

struct TimeCubeDataset {
    size_t index = 0;
    std::string name;
    std::string file;
    std::string category;
    bool enabled = false;
    bool local = false;
    bool indexed = false;
    bool recommended = false;
    std::string time_mode;
    std::string grain;
    std::string measure;
    std::string declared_date_field;
    int snapshot_year = -1;
    std::string exclusion_reason;
    size_t feature_count = 0;
    size_t matched_records = 0;
    size_t missing_date_records = 0;
    size_t out_of_range_records = 0;
    std::vector<std::pair<int, int>> years;
    std::vector<std::string> date_fields_found;
    std::string signature;
    std::string error;
};

struct TimeCubeResult {
    int year_from = 1900;
    int year_to = 2100;
    bool enabled_only = false;
    std::string layer_filter;
    std::vector<std::string> candidate_date_fields;
    std::vector<TimeCubeDataset> datasets;
};

class TimeCubeService {
public:
    explicit TimeCubeService(std::filesystem::path root);

    TimeCubeResult query(const std::vector<LayerDef>& layers, const TimeCubeQuery& query);
    nlohmann::json toJson(const TimeCubeResult& result) const;
    const std::vector<std::string>& candidateDateFields() const { return candidate_date_fields_; }

private:
    TimeCubeDataset indexDataset(size_t index, const LayerDef& layer, bool rebuild);
    bool loadCachedIndex(const std::filesystem::path& cache_path, const std::string& signature, TimeCubeDataset& out);
    void saveCachedIndex(const std::filesystem::path& cache_path, const TimeCubeDataset& dataset);
    std::filesystem::path cachePathFor(const std::string& file) const;
    void loadDatasetSpecs();

    struct DatasetTimeSpec {
        std::string mode;
        std::string date_field;
        std::string grain;
        std::string measure;
        std::string reason;
        int snapshot_year = -1;
        bool explicit_spec = false;
    };

    std::filesystem::path root_;
    std::vector<std::string> candidate_date_fields_;
    std::unordered_map<std::string, DatasetTimeSpec> dataset_specs_;
    std::string specs_signature_;
    std::mutex mutex_;
    std::unordered_map<std::string, TimeCubeDataset> memory_cache_;
};
