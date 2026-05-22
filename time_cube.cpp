#include "time_cube.h"

#include "app_utils.h"
#include "cache_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

static const char* timeCubeCategoryToString(LayerDef::Category c) {
    switch (c) {
        case LayerDef::Category::Housing: return "housing";
        case LayerDef::Category::PublicHealth: return "public_health";
        case LayerDef::Category::Infrastructure: return "infrastructure";
        case LayerDef::Category::Zoning: return "zoning";
        case LayerDef::Category::Safety: return "safety";
    }
    return "unknown";
}

static std::string timeCubeJsonValueToString(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) {
        std::ostringstream os;
        os << v.get<double>();
        return os.str();
    }
    if (v.is_null()) return "";
    return v.dump();
}

static int timeCubeExtractYearMaybe(const std::string& s) {
    for (size_t i = 0; i + 3 < s.size(); ++i) {
        if (!std::isdigit((unsigned char)s[i]) ||
            !std::isdigit((unsigned char)s[i + 1]) ||
            !std::isdigit((unsigned char)s[i + 2]) ||
            !std::isdigit((unsigned char)s[i + 3])) continue;
        const int y = (s[i] - '0') * 1000 + (s[i + 1] - '0') * 100 + (s[i + 2] - '0') * 10 + (s[i + 3] - '0');
        if (y >= 1900 && y <= 2100) return y;
    }
    return -1;
}

static bool vectorContains(const std::vector<std::string>& values, const std::string& needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

TimeCubeService::TimeCubeService(fs::path root)
    : root_(std::move(root)),
      candidate_date_fields_({
          "RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE",
          "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate",
          "PERMIT_DATE", "APPLIED_DATE", "ISSUED_DATE", "STATUS_DATE",
          "SALEDATE", "SALE_DATE", "YEAR", "Year", "year", "Data_Value_Year",
          "CrimeDateTime", "IssuedDate", "ExpirationDate", "HMDA_Year",
          "ReleaseYear", "TAX_SALE_YEAR", "REDEMPTION_DATE"
      }) {
    loadDatasetSpecs();
}

void TimeCubeService::loadDatasetSpecs() {
    const fs::path spec_path = root_ / "data" / "schemas" / "time_cube_datasets.json";
    specs_signature_ = fileSignature(spec_path);
    std::ifstream in(spec_path);
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }
    if (!j.contains("datasets") || !j["datasets"].is_object()) return;
    for (auto it = j["datasets"].begin(); it != j["datasets"].end(); ++it) {
        const json& row = it.value();
        DatasetTimeSpec spec;
        spec.mode = row.value("mode", std::string());
        spec.date_field = row.value("date_field", std::string());
        spec.grain = row.value("grain", std::string());
        spec.measure = row.value("measure", std::string());
        spec.reason = row.value("reason", std::string());
        spec.snapshot_year = row.value("snapshot_year", -1);
        spec.explicit_spec = true;
        dataset_specs_[it.key()] = std::move(spec);
    }
}

fs::path TimeCubeService::cachePathFor(const std::string& file) const {
    return root_ / "data" / "cache" / "time_cube" / (file + ".time_cube.json");
}

bool TimeCubeService::loadCachedIndex(const fs::path& cache_path, const std::string& signature, TimeCubeDataset& out) {
    std::ifstream in(cache_path);
    if (!in) return false;
    json j;
    try {
        in >> j;
    } catch (...) {
        return false;
    }
    if (j.value("schema_version", 0) != 2) return false;
    if (j.value("signature", std::string()) != signature) return false;
    if (j.value("specs_signature", std::string()) != specs_signature_) return false;
    out.local = true;
    out.indexed = true;
    out.signature = signature;
    out.time_mode = j.value("time_mode", std::string());
    out.grain = j.value("grain", std::string());
    out.measure = j.value("measure", std::string());
    out.declared_date_field = j.value("declared_date_field", std::string());
    out.snapshot_year = j.value("snapshot_year", -1);
    out.exclusion_reason = j.value("exclusion_reason", std::string());
    out.feature_count = j.value("feature_count", (size_t)0);
    out.matched_records = j.value("matched_records", (size_t)0);
    out.missing_date_records = j.value("missing_date_records", (size_t)0);
    out.date_fields_found.clear();
    if (j.contains("date_fields_found") && j["date_fields_found"].is_array()) {
        for (const auto& v : j["date_fields_found"]) {
            if (v.is_string()) out.date_fields_found.push_back(v.get<std::string>());
        }
    }
    out.years.clear();
    if (j.contains("years") && j["years"].is_array()) {
        for (const auto& row : j["years"]) {
            out.years.push_back({row.value("year", 0), row.value("count", 0)});
        }
    }
    out.recommended = out.matched_records > 0;
    return true;
}

void TimeCubeService::saveCachedIndex(const fs::path& cache_path, const TimeCubeDataset& dataset) {
    fs::create_directories(cache_path.parent_path());
    json j;
    j["schema_version"] = 2;
    j["signature"] = dataset.signature;
    j["specs_signature"] = specs_signature_;
    j["time_mode"] = dataset.time_mode;
    j["grain"] = dataset.grain;
    j["measure"] = dataset.measure;
    j["declared_date_field"] = dataset.declared_date_field;
    j["snapshot_year"] = dataset.snapshot_year;
    j["exclusion_reason"] = dataset.exclusion_reason;
    j["feature_count"] = dataset.feature_count;
    j["matched_records"] = dataset.matched_records;
    j["missing_date_records"] = dataset.missing_date_records;
    j["date_fields_found"] = dataset.date_fields_found;
    j["years"] = json::array();
    for (const auto& yc : dataset.years) {
        j["years"].push_back({{"year", yc.first}, {"count", yc.second}});
    }
    std::ofstream out(cache_path);
    if (out) out << j.dump(2);
}

TimeCubeDataset TimeCubeService::indexDataset(size_t index, const LayerDef& layer, bool rebuild) {
    TimeCubeDataset dataset;
    dataset.index = index;
    dataset.name = layer.name;
    dataset.file = layer.file;
    dataset.category = timeCubeCategoryToString(layer.category);
    dataset.enabled = layer.enabled;
    DatasetTimeSpec spec;
    auto spec_it = dataset_specs_.find(layer.file);
    if (spec_it != dataset_specs_.end()) spec = spec_it->second;
    dataset.time_mode = spec.explicit_spec ? spec.mode : "auto";
    dataset.grain = spec.grain;
    dataset.measure = spec.measure;
    dataset.declared_date_field = spec.date_field;
    dataset.snapshot_year = spec.snapshot_year;
    dataset.exclusion_reason = spec.reason;

    const fs::path layer_path = resolveStoredLayerPath(root_, layer);
    if (!fs::exists(layer_path)) {
        dataset.error = "local dataset file is missing";
        return dataset;
    }
    dataset.local = true;
    dataset.signature = fileSignature(layer_path);
    const fs::path cache_path = cachePathFor(layer.file);
    const std::string memory_key = layer.file + "|" + dataset.signature + "|" + specs_signature_;
    int inferred_dataset_year = timeCubeExtractYearMaybe(layer.file);
    if (inferred_dataset_year < 0) inferred_dataset_year = timeCubeExtractYearMaybe(layer.name);
    if (inferred_dataset_year < 0) inferred_dataset_year = timeCubeExtractYearMaybe(layer.description);
    if (inferred_dataset_year < 0) inferred_dataset_year = timeCubeExtractYearMaybe(layer.source_url);
    if (spec.snapshot_year >= 1900 && spec.snapshot_year <= 2100) inferred_dataset_year = spec.snapshot_year;

    if (spec.mode == "exclude") {
        dataset.indexed = true;
        dataset.recommended = false;
        std::lock_guard<std::mutex> lk(mutex_);
        memory_cache_[memory_key] = dataset;
        return dataset;
    }

    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = memory_cache_.find(memory_key);
        if (!rebuild && it != memory_cache_.end()) {
            TimeCubeDataset cached = it->second;
            cached.index = index;
            cached.name = layer.name;
            cached.category = timeCubeCategoryToString(layer.category);
            cached.enabled = layer.enabled;
            cached.time_mode = dataset.time_mode;
            cached.grain = dataset.grain;
            cached.measure = dataset.measure;
            cached.declared_date_field = dataset.declared_date_field;
            cached.snapshot_year = dataset.snapshot_year;
            cached.exclusion_reason = dataset.exclusion_reason;
            return cached;
        }
    }

    if (!rebuild && loadCachedIndex(cache_path, dataset.signature, dataset)) {
        dataset.index = index;
        dataset.name = layer.name;
        dataset.file = layer.file;
        dataset.category = timeCubeCategoryToString(layer.category);
        dataset.enabled = layer.enabled;
        dataset.time_mode = spec.explicit_spec ? spec.mode : dataset.time_mode;
        dataset.grain = spec.grain.empty() ? dataset.grain : spec.grain;
        dataset.measure = spec.measure.empty() ? dataset.measure : spec.measure;
        dataset.declared_date_field = spec.date_field.empty() ? dataset.declared_date_field : spec.date_field;
        dataset.snapshot_year = spec.snapshot_year >= 0 ? spec.snapshot_year : dataset.snapshot_year;
        dataset.exclusion_reason = spec.reason.empty() ? dataset.exclusion_reason : spec.reason;
        std::lock_guard<std::mutex> lk(mutex_);
        memory_cache_[memory_key] = dataset;
        return dataset;
    }

    try {
        std::ifstream in(layer_path);
        json gj;
        in >> gj;
        if (!gj.contains("features") || !gj["features"].is_array()) {
            dataset.error = "dataset has no GeoJSON features array";
            return dataset;
        }

        std::vector<int> counts(2101, 0);
        for (const auto& feature : gj["features"]) {
            ++dataset.feature_count;
            const json* props = feature.contains("properties") ? &feature["properties"] : nullptr;
            if (!props || !props->is_object()) {
                ++dataset.missing_date_records;
                continue;
            }

            std::string date_value;
            if (!spec.date_field.empty()) {
                auto it = props->find(spec.date_field);
                if (it != props->end()) {
                    date_value = timeCubeJsonValueToString(*it);
                    if (!date_value.empty() && !vectorContains(dataset.date_fields_found, spec.date_field)) {
                        dataset.date_fields_found.push_back(spec.date_field);
                    }
                }
            }
            for (const std::string& field : candidate_date_fields_) {
                if (!date_value.empty()) break;
                auto it = props->find(field);
                if (it == props->end()) continue;
                const std::string v = timeCubeJsonValueToString(*it);
                if (v.empty()) continue;
                date_value = v;
                if (!vectorContains(dataset.date_fields_found, field)) dataset.date_fields_found.push_back(field);
                break;
            }

            const int year = timeCubeExtractYearMaybe(date_value);
            if (year < 0 && inferred_dataset_year < 0) {
                ++dataset.missing_date_records;
                continue;
            }
            const int resolved_year = year >= 0 ? year : inferred_dataset_year;
            if (year < 0 && inferred_dataset_year >= 0) {
                const char* source = spec.snapshot_year >= 0 ? "snapshot_year" : "inferred_dataset_year";
                if (!vectorContains(dataset.date_fields_found, source)) dataset.date_fields_found.push_back(source);
            }
            ++counts[(size_t)resolved_year];
            ++dataset.matched_records;
        }
        for (int year = 1900; year <= 2100; ++year) {
            const int count = counts[(size_t)year];
            if (count > 0) dataset.years.push_back({year, count});
        }
        dataset.indexed = true;
        dataset.recommended = dataset.matched_records > 0;
        saveCachedIndex(cache_path, dataset);
    } catch (const std::exception& e) {
        dataset.error = e.what();
    }

    std::lock_guard<std::mutex> lk(mutex_);
    memory_cache_[memory_key] = dataset;
    return dataset;
}

TimeCubeResult TimeCubeService::query(const std::vector<LayerDef>& layers, const TimeCubeQuery& query) {
    TimeCubeResult result;
    result.year_from = std::clamp(query.year_from, 1900, 2100);
    result.year_to = std::clamp(query.year_to, 1900, 2100);
    if (result.year_from > result.year_to) std::swap(result.year_from, result.year_to);
    result.enabled_only = query.enabled_only;
    result.layer_filter = query.layer_filter;
    result.candidate_date_fields = candidate_date_fields_;

    for (size_t i = 0; i < layers.size(); ++i) {
        const LayerDef& layer = layers[i];
        if (!query.layer_filter.empty() && layer.file != query.layer_filter && layer.name != query.layer_filter) continue;
        if (query.enabled_only && !layer.enabled) continue;

        TimeCubeDataset dataset = indexDataset(i, layer, query.rebuild);
        size_t matched_in_range = 0;
        size_t out_of_range = 0;
        std::vector<std::pair<int, int>> years_in_range;
        for (const auto& yc : dataset.years) {
            if (yc.first < result.year_from || yc.first > result.year_to) {
                out_of_range += (size_t)std::max(0, yc.second);
                continue;
            }
            years_in_range.push_back(yc);
            matched_in_range += (size_t)std::max(0, yc.second);
        }
        dataset.out_of_range_records = out_of_range;
        dataset.matched_records = matched_in_range;
        dataset.years = std::move(years_in_range);
        dataset.recommended = dataset.matched_records > 0 || !dataset.date_fields_found.empty();
        result.datasets.push_back(std::move(dataset));
    }
    return result;
}

json TimeCubeService::toJson(const TimeCubeResult& result) const {
    json out;
    out["schema_version"] = 2;
    out["grain"] = "dataset_year";
    out["date_fields"] = result.candidate_date_fields;
    out["filters"] = {
        {"layer", result.layer_filter},
        {"from", result.year_from},
        {"to", result.year_to},
        {"enabled_only", result.enabled_only}
    };
    out["datasets"] = json::array();
    for (const auto& d : result.datasets) {
        json row;
        row["index"] = d.index;
        row["name"] = d.name;
        row["file"] = d.file;
        row["category"] = d.category;
        row["enabled"] = d.enabled;
        row["local"] = d.local;
        row["indexed"] = d.indexed;
        row["recommended_for_time_cube"] = d.recommended;
        row["time_mode"] = d.time_mode;
        row["grain"] = d.grain;
        row["measure"] = d.measure;
        row["declared_date_field"] = d.declared_date_field;
        row["snapshot_year"] = d.snapshot_year;
        row["exclusion_reason"] = d.exclusion_reason;
        row["feature_count"] = d.feature_count;
        row["matched_records"] = d.matched_records;
        row["missing_date_records"] = d.missing_date_records;
        row["out_of_range_records"] = d.out_of_range_records;
        row["date_fields_found"] = d.date_fields_found;
        row["signature"] = d.signature;
        row["years"] = json::array();
        for (const auto& yc : d.years) row["years"].push_back({{"year", yc.first}, {"count", yc.second}});
        if (!d.error.empty()) row["error"] = d.error;
        out["datasets"].push_back(std::move(row));
    }
    return out;
}
