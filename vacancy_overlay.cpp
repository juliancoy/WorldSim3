#include "vacancy_overlay.h"

#include "app_utils.h"
#include "feature_props.h"
#include "layer_geometry.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

void saveDerivedVacancyStatus(
    const fs::path& out_path,
    const std::vector<LayerDef::FeatureGeom>& parcel_features,
    const std::vector<int>& notice_counts,
    const std::vector<int>& rehab_counts,
    size_t vacant_notice_rows_total,
    size_t vacant_rehab_rows_total,
    size_t vacant_notice_rows_matched,
    size_t vacant_rehab_rows_matched) {
    if (parcel_features.size() != notice_counts.size() || parcel_features.size() != rehab_counts.size()) return;
    json j;
    j["schema_version"] = 1;
    j["parcel_feature_count"] = parcel_features.size();
    j["vacant_notice_rows_total"] = vacant_notice_rows_total;
    j["vacant_rehab_rows_total"] = vacant_rehab_rows_total;
    j["vacant_notice_rows_matched"] = vacant_notice_rows_matched;
    j["vacant_rehab_rows_matched"] = vacant_rehab_rows_matched;
    j["vacant_notice_rows_unmatched"] = vacant_notice_rows_total - std::min(vacant_notice_rows_total, vacant_notice_rows_matched);
    j["vacant_rehab_rows_unmatched"] = vacant_rehab_rows_total - std::min(vacant_rehab_rows_total, vacant_rehab_rows_matched);
    j["entries"] = json::array();
    auto local_norm = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char ch : s) {
            unsigned char u = (unsigned char)ch;
            if (std::isalnum(u)) out.push_back((char)std::toupper(u));
        }
        return out;
    };
    auto local_prop = [](const LayerDef::FeatureGeom& fg, const char* key) {
        for (const auto& kv : fg.properties) {
            if (kv.first == key) return kv.second;
        }
        return std::string{};
    };

    for (size_t i = 0; i < parcel_features.size(); ++i) {
        int n = notice_counts[i];
        int r = rehab_counts[i];
        int w = n + r;
        if (w <= 0) continue;
        std::string bl = local_norm(local_prop(parcel_features[i], "BLOCKLOT"));
        j["entries"].push_back({
            {"feature_index", i},
            {"blocklot_norm", bl},
            {"vacant_notice_count", n},
            {"vacant_rehab_count", r},
            {"vacancy_weight", w}
        });
    }
    fs::create_directories(out_path.parent_path());
    std::ofstream out(out_path);
    if (out) out << j.dump();
}

int runVacancySelftest(const fs::path& root) {
    const fs::path parcel_path = resolveStoredLayerPathForFile(root, "parcel.geojson");
    const fs::path notice_path = resolveStoredLayerPathForFile(root, "vacant_building_notices.geojson");
    const fs::path rehab_path = resolveStoredLayerPathForFile(root, "vacant_building_rehabs.geojson");

    json out;
    out["mode"] = "vacancy-selftest";
    out["root"] = root.string();
    out["paths"] = {
        {"parcel", parcel_path.string()},
        {"vacant_notices", notice_path.string()},
        {"vacant_rehabs", rehab_path.string()}
    };

    if (!fs::exists(parcel_path) || !fs::exists(notice_path) || !fs::exists(rehab_path)) {
        out["ok"] = false;
        out["error"] = "required layer file missing";
        out["exists"] = {
            {"parcel", fs::exists(parcel_path)},
            {"vacant_notices", fs::exists(notice_path)},
            {"vacant_rehabs", fs::exists(rehab_path)}
        };
        std::printf("%s\n", out.dump(2).c_str());
        return 2;
    }

    auto parcels = loadLayerPointsFromFile(parcel_path);
    auto notices = loadLayerPointsFromFile(notice_path);
    auto rehabs = loadLayerPointsFromFile(rehab_path);

    std::unordered_map<std::string, std::vector<size_t>> parcel_by_blocklot;
    parcel_by_blocklot.reserve(parcels.size());
    for (size_t i = 0; i < parcels.size(); ++i) {
        std::string bl = normalizeJoinKey(getPropertyValue(parcels[i], "BLOCKLOT"));
        if (!bl.empty()) parcel_by_blocklot[bl].push_back(i);
    }

    std::vector<int> notice_by_parcel(parcels.size(), 0);
    std::vector<int> rehab_by_parcel(parcels.size(), 0);
    size_t notice_rows_matched = 0, notice_rows_unmatched = 0, notice_rows_missing_key = 0;
    size_t rehab_rows_matched = 0, rehab_rows_unmatched = 0, rehab_rows_missing_key = 0;

    for (const auto& fg : notices) {
        std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
        if (bl.empty()) {
            notice_rows_missing_key++;
            continue;
        }
        auto it = parcel_by_blocklot.find(bl);
        if (it == parcel_by_blocklot.end()) {
            notice_rows_unmatched++;
            continue;
        }
        notice_rows_matched++;
        for (size_t idx : it->second) notice_by_parcel[idx] += 1;
    }

    for (const auto& fg : rehabs) {
        std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
        if (bl.empty()) {
            rehab_rows_missing_key++;
            continue;
        }
        auto it = parcel_by_blocklot.find(bl);
        if (it == parcel_by_blocklot.end()) {
            rehab_rows_unmatched++;
            continue;
        }
        rehab_rows_matched++;
        for (size_t idx : it->second) rehab_by_parcel[idx] += 1;
    }

    size_t parcels_with_notice = 0, parcels_with_rehab = 0, parcels_with_any = 0;
    size_t parcels_with_geometry = 0, parcels_with_polygon_rings = 0, parcels_with_nonzero_extent = 0;
    size_t matched_parcels_with_polygon_geometry = 0;
    int max_notice = 0, max_rehab = 0, max_weight = 0;
    for (size_t i = 0; i < parcels.size(); ++i) {
        int n = notice_by_parcel[i];
        int r = rehab_by_parcel[i];
        int w = n + r;
        const auto& ex = parcels[i].extent;
        const bool nonzero_extent = (ex.max_lon > ex.min_lon) && (ex.max_lat > ex.min_lat);
        const bool has_poly = !parcels[i].rings.empty();
        const bool has_geom = has_poly || nonzero_extent;
        if (has_geom) parcels_with_geometry++;
        if (has_poly) parcels_with_polygon_rings++;
        if (nonzero_extent) parcels_with_nonzero_extent++;
        if (n > 0) parcels_with_notice++;
        if (r > 0) parcels_with_rehab++;
        if (w > 0) parcels_with_any++;
        if (w > 0 && has_poly) matched_parcels_with_polygon_geometry++;
        max_notice = std::max(max_notice, n);
        max_rehab = std::max(max_rehab, r);
        max_weight = std::max(max_weight, w);
    }

    json counts = json::object();
    counts["parcel_features"] = parcels.size();
    counts["vacant_notice_rows"] = notices.size();
    counts["vacant_rehab_rows"] = rehabs.size();
    counts["parcel_unique_blocklot_keys"] = parcel_by_blocklot.size();
    counts["notice_rows_matched"] = notice_rows_matched;
    counts["notice_rows_unmatched"] = notice_rows_unmatched;
    counts["notice_rows_missing_key"] = notice_rows_missing_key;
    counts["rehab_rows_matched"] = rehab_rows_matched;
    counts["rehab_rows_unmatched"] = rehab_rows_unmatched;
    counts["rehab_rows_missing_key"] = rehab_rows_missing_key;
    counts["parcels_with_notice"] = parcels_with_notice;
    counts["parcels_with_rehab"] = parcels_with_rehab;
    counts["parcels_with_any_vacancy"] = parcels_with_any;
    counts["parcels_with_geometry"] = parcels_with_geometry;
    counts["parcels_with_polygon_rings"] = parcels_with_polygon_rings;
    counts["parcels_with_nonzero_extent"] = parcels_with_nonzero_extent;
    counts["matched_parcels_with_polygon_geometry"] = matched_parcels_with_polygon_geometry;
    counts["geometry_attainable"] = (matched_parcels_with_polygon_geometry > 0);
    counts["max_notice_count_on_parcel"] = max_notice;
    counts["max_rehab_count_on_parcel"] = max_rehab;
    counts["max_total_vacancy_weight_on_parcel"] = max_weight;
    out["counts"] = std::move(counts);
    out["ok"] = (parcels_with_any > 0) &&
                (notice_rows_matched + rehab_rows_matched > 0) &&
                (matched_parcels_with_polygon_geometry > 0);
    std::printf("%s\n", out.dump(2).c_str());
    return out["ok"].get<bool>() ? 0 : 3;
}
