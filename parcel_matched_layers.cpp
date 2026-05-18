#include "parcel_matched_layers.h"

#include "app_utils.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct MatchSpec {
    const char* source;
    const char* output;
    const char* name;
    const char* event_kind;
    std::vector<const char*> date_fields;
    std::vector<const char*> id_fields;
    std::vector<const char*> summary_fields;
};

const std::vector<MatchSpec>& specs() {
    static const std::vector<MatchSpec> kSpecs = {
        {"open_notices_vacant.geojson", "open_notices_vacant_parcels.geojson", "Open Notices - Vacant Code Parcels", "open_notice_vacant", {"Issue_Date_ISO", "Issue_Year"}, {"ObjectID"}, {"Notice_Type", "Council_District", "Councilperson", "Impact_Investment_Area", "Neighborhood"}},
        {"open_notices_exterior.geojson", "open_notices_exterior_parcels.geojson", "Open Notices - Exterior Code Parcels", "open_notice_exterior", {"Issue_Date_ISO", "Issue_Year"}, {"ObjectID"}, {"Notice_Type", "Council_District", "Councilperson", "Impact_Investment_Area", "Neighborhood"}},
        {"open_notices_interior.geojson", "open_notices_interior_parcels.geojson", "Open Notices - Interior Code Parcels", "open_notice_interior", {"Issue_Date_ISO", "Issue_Year"}, {"ObjectID"}, {"Notice_Type", "Council_District", "Councilperson", "Impact_Investment_Area", "Neighborhood"}},
        {"open_notices_interior_exterior.geojson", "open_notices_interior_exterior_parcels.geojson", "Open Notices - Interior/Exterior Code Parcels", "open_notice_interior_exterior", {"Issue_Date_ISO", "Issue_Year"}, {"ObjectID"}, {"Notice_Type", "Council_District", "Councilperson", "Impact_Investment_Area", "Neighborhood"}},
        {"open_notices_other.geojson", "open_notices_other_parcels.geojson", "Open Notices - Other Code Parcels", "open_notice_other", {"Issue_Date_ISO", "Issue_Year"}, {"ObjectID"}, {"Notice_Type", "Council_District", "Councilperson", "Impact_Investment_Area", "Neighborhood"}},
        {"codemap_recently_rehabbed_vacant_buildings.geojson", "codemap_recently_rehabbed_vacant_building_parcels.geojson", "CodeMap Recently Rehabbed Vacant Building Parcels", "codemap_recent_rehab", {"DateIssue_ISO"}, {"OBJECTID", "PermitNum"}, {"PermitNum", "VBN", "csm_exist_use", "csm_prop_use", "Council_District", "Neighborhood", "Typology2017"}},
    };
    return kSpecs;
}

std::string jsonValueToText(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) {
        std::ostringstream ss;
        ss << v.get<double>();
        return ss.str();
    }
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_null()) return "";
    return v.dump();
}

std::string trimCopy(const std::string& s) {
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) ++first;
    size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) --last;
    return s.substr(first, last - first);
}

std::string normalizedKeyFromText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        if (!std::isspace(ch)) out.push_back(static_cast<char>(std::toupper(ch)));
    }
    return out;
}

bool firstPresent(const json& props, const std::vector<const char*>& fields, std::string& out) {
    if (!props.is_object()) return false;
    for (const char* field : fields) {
        auto it = props.find(field);
        if (it == props.end() || it->is_null()) continue;
        std::string text = trimCopy(jsonValueToText(*it));
        if (!text.empty()) {
            out = text;
            return true;
        }
    }
    return false;
}

const json& objectOrEmpty(const json& parent, const char* key) {
    static const json kEmptyObject = json::object();
    auto it = parent.find(key);
    if (it != parent.end() && it->is_object()) return *it;
    return kEmptyObject;
}

std::string sourceBlocklot(const json& props) {
    std::string value;
    if (firstPresent(props, {"BLOCKLOT", "BlockLot", "blocklot", "Block_Lot", "BLOCK_LOT"}, value)) {
        return normalizedKeyFromText(value);
    }
    std::string block;
    std::string lot;
    if (firstPresent(props, {"Block", "BLOCK", "block"}, block) &&
        firstPresent(props, {"Lot", "LOT", "lot"}, lot)) {
        return normalizedKeyFromText(block + lot);
    }
    return {};
}

std::string safeSummaryFieldName(std::string field) {
    for (char& ch : field) {
        if (ch == '/' || ch == ' ') ch = '_';
    }
    return field;
}

json mergeEvents(const json& parcel_feature, const std::vector<const json*>& events, const MatchSpec& spec) {
    json out_props = objectOrEmpty(parcel_feature, "properties");
    out_props["matched_layer"] = spec.source;
    out_props["matched_event_kind"] = spec.event_kind;
    out_props["matched_event_count"] = events.size();

    std::vector<std::string> dates;
    std::vector<std::string> ids;
    for (const json* event : events) {
        const json& props = objectOrEmpty(*event, "properties");
        std::string date_value;
        if (firstPresent(props, spec.date_fields, date_value)) dates.push_back(date_value);

        std::vector<std::string> id_parts;
        for (const char* field : spec.id_fields) {
            auto it = props.find(field);
            if (it == props.end() || it->is_null()) continue;
            std::string text = trimCopy(jsonValueToText(*it));
            if (!text.empty()) id_parts.push_back(text);
        }
        if (!id_parts.empty()) {
            std::string joined = id_parts.front();
            for (size_t i = 1; i < id_parts.size(); ++i) joined += " / " + id_parts[i];
            ids.push_back(std::move(joined));
        }
    }

    if (!dates.empty()) {
        std::sort(dates.begin(), dates.end());
        dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
        out_props["matched_first_date"] = dates.front();
        out_props["matched_latest_date"] = dates.back();
    }
    if (!ids.empty()) {
        std::vector<std::string> unique_ids;
        std::unordered_set<std::string> seen;
        for (const std::string& id : ids) {
            if (seen.insert(id).second) unique_ids.push_back(id);
        }
        out_props["matched_first_id"] = unique_ids.front();
        std::string sample;
        const size_t n = std::min<size_t>(5, unique_ids.size());
        for (size_t i = 0; i < n; ++i) {
            if (i) sample += "; ";
            sample += unique_ids[i];
        }
        out_props["matched_ids_sample"] = sample;
    }

    for (const char* field : spec.summary_fields) {
        std::vector<std::string> values;
        std::unordered_set<std::string> seen;
        for (const json* event : events) {
            const json& props = objectOrEmpty(*event, "properties");
            auto it = props.find(field);
            if (it == props.end() || it->is_null()) continue;
            std::string text = trimCopy(jsonValueToText(*it));
            if (!text.empty() && seen.insert(text).second) values.push_back(text);
        }
        if (!values.empty()) {
            std::string sample;
            const size_t n = std::min<size_t>(5, values.size());
            for (size_t i = 0; i < n; ++i) {
                if (i) sample += "; ";
                sample += values[i];
            }
            out_props["matched_" + safeSummaryFieldName(field) + "_sample"] = sample;
        }
    }

    json out;
    out["type"] = "Feature";
    out["properties"] = std::move(out_props);
    auto geom_it = parcel_feature.find("geometry");
    out["geometry"] = geom_it != parcel_feature.end() ? *geom_it : json(nullptr);
    return out;
}

bool readJsonFile(const fs::path& path, json& out, std::ostream* log) {
    std::ifstream in(path);
    if (!in) {
        if (log) *log << "[parcel-match] missing " << path.string() << "\n";
        return false;
    }
    try {
        in >> out;
    } catch (const std::exception& e) {
        if (log) *log << "[parcel-match] failed to parse " << path.string() << ": " << e.what() << "\n";
        return false;
    }
    return true;
}

bool outputIsFresh(const fs::path& output, const fs::path& parcel, const fs::path& source) {
    std::error_code ec;
    if (!fs::exists(output, ec)) return false;
    const auto out_time = fs::last_write_time(output, ec);
    if (ec) return false;
    const auto parcel_time = fs::last_write_time(parcel, ec);
    if (ec) return false;
    const auto source_time = fs::last_write_time(source, ec);
    if (ec) return false;
    return out_time >= parcel_time && out_time >= source_time;
}

} // namespace

std::vector<ParcelMatchedLayerBuildStat> ensureParcelMatchedEventLayers(
    const fs::path& root,
    bool force,
    std::ostream* log) {
    const fs::path parcel_path = resolveStoredLayerPathForFile(root, "parcel.geojson");
    std::vector<ParcelMatchedLayerBuildStat> stats;

    json parcel_collection;
    if (!readJsonFile(parcel_path, parcel_collection, log)) return stats;
    auto parcels_it = parcel_collection.find("features");
    if (parcels_it == parcel_collection.end() || !parcels_it->is_array()) return stats;
    const json& parcels = *parcels_it;

    std::unordered_map<std::string, const json*> parcel_by_key;
    parcel_by_key.reserve(parcels.size());
    for (const json& parcel : parcels) {
        const json& props = objectOrEmpty(parcel, "properties");
        std::string key = sourceBlocklot(props);
        if (!key.empty() && parcel_by_key.find(key) == parcel_by_key.end()) {
            parcel_by_key.emplace(std::move(key), &parcel);
        }
    }

    for (const MatchSpec& spec : specs()) {
        ParcelMatchedLayerBuildStat stat;
        stat.source = spec.source;
        stat.output = spec.output;
        const fs::path source_path = resolveStoredLayerPathForFile(root, spec.source);
        const fs::path out_path = resolveStoredLayerPathForFile(root, spec.output);

        if (!force && outputIsFresh(out_path, parcel_path, source_path)) {
            stat.skipped_up_to_date = true;
            if (log) *log << "[parcel-match] up to date " << spec.output << "\n";
            stats.push_back(stat);
            continue;
        }

        json source_collection;
        if (!readJsonFile(source_path, source_collection, log)) {
            stats.push_back(stat);
            continue;
        }
        auto source_features_it = source_collection.find("features");
        if (source_features_it == source_collection.end() || !source_features_it->is_array()) {
            stats.push_back(stat);
            continue;
        }
        const json& source_features = *source_features_it;
        stat.source_events = source_features.size();

        std::unordered_map<std::string, std::vector<const json*>> events_by_key;
        std::vector<std::string> event_key_order;
        events_by_key.reserve(source_features.size());
        for (const json& event : source_features) {
            const json& props = objectOrEmpty(event, "properties");
            std::string key = sourceBlocklot(props);
            if (key.empty()) {
                ++stat.missing_keys;
                continue;
            }
            auto [it, inserted] = events_by_key.emplace(key, std::vector<const json*>{});
            if (inserted) event_key_order.push_back(key);
            it->second.push_back(&event);
        }

        json out_features = json::array();
        for (const std::string& key : event_key_order) {
            const auto& events = events_by_key.at(key);
            auto parcel_it = parcel_by_key.find(key);
            if (parcel_it == parcel_by_key.end()) {
                stat.unmatched_events += events.size();
                continue;
            }
            out_features.push_back(mergeEvents(*parcel_it->second, events, spec));
        }
        stat.matched_parcels = out_features.size();

        json collection;
        collection["type"] = "FeatureCollection";
        collection["name"] = spec.name;
        collection["source"] = spec.source;
        collection["join_key"] = "BLOCKLOT";
        collection["source_feature_count"] = stat.source_events;
        collection["matched_parcel_count"] = stat.matched_parcels;
        collection["unmatched_source_event_count"] = stat.unmatched_events;
        collection["missing_source_key_count"] = stat.missing_keys;
        collection["features"] = std::move(out_features);

        std::ofstream out(out_path);
        if (!out) {
            if (log) *log << "[parcel-match] failed to write " << out_path.string() << "\n";
            stats.push_back(stat);
            continue;
        }
        out << collection.dump();
        stat.written = true;
        if (log) {
            *log << "[parcel-match] " << out_path.string()
                 << ": parcels=" << stat.matched_parcels
                 << " source_events=" << stat.source_events
                 << " unmatched_events=" << stat.unmatched_events
                 << " missing_key=" << stat.missing_keys << "\n";
        }
        stats.push_back(stat);
    }
    return stats;
}
