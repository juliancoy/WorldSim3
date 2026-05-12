#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
struct SourceSpec {
    std::string jurisdiction;
    fs::path path;
};

const std::vector<std::pair<std::string, std::vector<std::string>>> kFieldAliases = {
    {"source_parcel_id", {"source_parcel_id", "ACCOUNTID", "ACCOUNT_ID", "ACCTID", "ACCT_ID", "ACCOUNT", "ACCOUNTNO", "TAXPIN", "PARCELID", "PARCEL_ID", "PARCELNO", "PIN", "PARCEL_ASSET_ID", "MAP_PARCEL", "MAPLOT", "OBJECTID", "OBJECTID_1"}},
    {"account_id", {"account_id", "ACCOUNTID", "ACCOUNT_ID", "ACCTID", "ACCT_ID", "ACCOUNT", "ACCOUNTNO", "TAXPIN", "SDAT_ACCOUNT"}},
    {"blocklot", {"blocklot", "BLOCKLOT", "BLOCK_LOT", "BlockLot", "PIN", "PARCELID", "PARCEL_ID", "MAPLOT"}},
    {"address", {"address", "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD", "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1", "SITE_ADDR", "SITUSADDR", "LOCATION", "Location", "SITUS_ADDRESS"}},
    {"owner", {"owner", "owner_name", "FULL_OWNER_NAME", "OWNER_NA1", "OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR", "OWNNAME", "OWNNAME1"}},
    {"land_value", {"land_value", "CURRLAND", "LAND_VALUE", "LANDVAL", "LAND_VAL"}},
    {"improvement_value", {"improvement_value", "CURRIMPR", "IMPR_VALUE", "IMPROVEMENT_VALUE", "IMPROVEVAL", "BLDG_VALUE"}},
    {"current_value", {"current_value", "TAXBASE", "ARTAXBAS", "TOTAL_VALUE", "TOTALVAL", "NFMTTLVL", "ASSD_VALUE", "ASSESSMENT", "MARKET_VALUE"}},
    {"sale_price", {"sale_price", "SALEPRIC", "SALE_PRICE", "SALEAMT", "SALE_AMOUNT"}},
    {"sale_date", {"sale_date", "SALEDATE", "SALE_DATE", "LAST_SALE_DATE"}},
    {"year_built", {"year_built", "YEAR_BUILD", "YEARBUILT", "YR_BUILT", "BUILT", "BLDG_YEAR"}},
    {"sdat_link", {"sdat_link", "SDATLINK", "SDAT_LINK", "PROPERTY_LINK"}},
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string cleanText(const json& v) {
    if (v.is_null()) return {};
    std::string s;
    if (v.is_string()) s = v.get<std::string>();
    else if (v.is_number_integer()) s = std::to_string(v.get<long long>());
    else if (v.is_number_unsigned()) s = std::to_string(v.get<unsigned long long>());
    else if (v.is_number_float()) s = std::to_string(v.get<double>());
    else if (v.is_boolean()) s = v.get<bool>() ? "true" : "false";
    else s = v.dump();

    auto is_space = [](unsigned char c) { return std::isspace(c); };
    while (!s.empty() && is_space((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && is_space((unsigned char)s.back())) s.pop_back();
    std::string out;
    out.reserve(s.size());
    bool prev_space = false;
    for (unsigned char c : s) {
        if (is_space(c)) {
            if (!prev_space) out.push_back(' ');
            prev_space = true;
        } else {
            out.push_back((char)c);
            prev_space = false;
        }
    }
    return out;
}

std::string normKey(const json& v) {
    const std::string s = cleanText(v);
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c)) out.push_back((char)std::toupper(c));
    }
    return out;
}

json firstProp(const json& props, const std::vector<std::string>& keys) {
    if (!props.is_object()) return json();
    std::unordered_map<std::string, std::string> by_lower;
    for (auto it = props.begin(); it != props.end(); ++it) by_lower.emplace(lower(it.key()), it.key());
    for (const auto& key : keys) {
        auto direct = props.find(key);
        if (direct != props.end() && !direct->is_null() && cleanText(*direct) != "") return *direct;
        auto folded = by_lower.find(lower(key));
        if (folded != by_lower.end()) {
            const auto& v = props.at(folded->second);
            if (!v.is_null() && cleanText(v) != "") return v;
        }
    }
    return json();
}

double parseNumber(const json& v) {
    if (v.is_number()) return v.get<double>();
    const std::string s = cleanText(v);
    std::string filtered;
    filtered.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isdigit(c) || c == '.' || c == '-') filtered.push_back((char)c);
    }
    if (filtered.empty() || filtered == "-" || filtered == ".") return 0.0;
    try {
        return std::stod(filtered);
    } catch (...) {
        return 0.0;
    }
}

json readFeatureCollection(const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("failed to open " + path.string());
    json j;
    in >> j;
    if (!j.is_object() || j.value("type", "") != "FeatureCollection" || !j.contains("features") || !j["features"].is_array()) {
        throw std::runtime_error(path.string() + " is not a GeoJSON FeatureCollection");
    }
    return j;
}

json canonicalProps(const std::string& jurisdiction, const fs::path& source_file, const json& props, const json* property_props = nullptr) {
    json merged = props.is_object() ? props : json::object();
    if (property_props && property_props->is_object()) {
        for (auto it = property_props->begin(); it != property_props->end(); ++it) {
            if (!merged.contains(it.key()) || merged[it.key()].is_null() || cleanText(merged[it.key()]).empty()) {
                merged[it.key()] = it.value();
            }
        }
    }

    json out = merged;
    out["jurisdiction"] = jurisdiction;
    out["source_file"] = source_file.filename().string();
    for (const auto& [field, aliases] : kFieldAliases) {
        json raw = firstProp(merged, aliases);
        if (field == "land_value" || field == "improvement_value" || field == "current_value" || field == "sale_price") {
            out[field] = parseNumber(raw);
        } else if (field == "year_built") {
            const double n = parseNumber(raw);
            out[field] = n > 0 ? (int)n : 0;
        } else {
            out[field] = cleanText(raw);
        }
    }
    if (cleanText(out["blocklot"]).empty()) {
        const std::string source_id = cleanText(out["source_parcel_id"]);
        out["blocklot"] = !source_id.empty() ? source_id : cleanText(out["account_id"]);
    }
    const std::string id_key = normKey(out["blocklot"].is_null() ? json(cleanText(out["source_parcel_id"])) : out["blocklot"]);
    const std::string fallback_key = normKey(out["account_id"]);
    out["regional_parcel_id"] = jurisdiction + ":" + (!id_key.empty() ? id_key : fallback_key);
    return out;
}

SourceSpec parseSourceSpec(const std::string& raw) {
    const size_t pos = raw.find(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= raw.size()) {
        throw std::runtime_error("source specs must be Jurisdiction:path.geojson: " + raw);
    }
    return {raw.substr(0, pos), fs::path(raw.substr(pos + 1))};
}

void usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " --input Jurisdiction:path.geojson [--property-input Jurisdiction:path.geojson] --output data/layers/regional_parcels.geojson\n";
}
}

int main(int argc, char** argv) {
    try {
        std::vector<SourceSpec> inputs;
        std::vector<SourceSpec> property_inputs;
        fs::path output = "data/layers/regional_parcels.geojson";
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto need_value = [&](const char* name) -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
                return argv[++i];
            };
            if (arg == "--input") inputs.push_back(parseSourceSpec(need_value("--input")));
            else if (arg == "--property-input") property_inputs.push_back(parseSourceSpec(need_value("--property-input")));
            else if (arg == "--output") output = need_value("--output");
            else if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
            else throw std::runtime_error("unknown argument: " + arg);
        }
        if (inputs.empty()) {
            usage(argv[0]);
            return 2;
        }

        std::unordered_map<std::string, std::unordered_map<std::string, json>> property_indexes;
        for (const auto& spec : property_inputs) {
            json fc = readFeatureCollection(spec.path);
            auto& index = property_indexes[spec.jurisdiction];
            for (const auto& feature : fc["features"]) {
                const json props = feature.value("properties", json::object());
                const json key_props = canonicalProps(spec.jurisdiction, spec.path, props);
                std::string key = normKey(key_props["blocklot"]);
                if (key.empty()) key = normKey(key_props["source_parcel_id"]);
                if (key.empty()) key = normKey(key_props["account_id"]);
                if (!key.empty() && index.find(key) == index.end()) index.emplace(std::move(key), props);
            }
        }

        json features = json::array();
        json counts = json::object();
        for (const auto& spec : inputs) {
            json fc = readFeatureCollection(spec.path);
            counts[spec.jurisdiction] = fc["features"].size();
            for (const auto& feature : fc["features"]) {
                if (!feature.is_object() || !feature.contains("geometry") || feature["geometry"].is_null()) continue;
                const json props = feature.value("properties", json::object());
                const json base_key_props = canonicalProps(spec.jurisdiction, spec.path, props);
                std::string key = normKey(base_key_props["blocklot"]);
                if (key.empty()) key = normKey(base_key_props["source_parcel_id"]);
                if (key.empty()) key = normKey(base_key_props["account_id"]);
                const json* joined = nullptr;
                auto jur_it = property_indexes.find(spec.jurisdiction);
                if (jur_it != property_indexes.end()) {
                    auto prop_it = jur_it->second.find(key);
                    if (prop_it != jur_it->second.end()) joined = &prop_it->second;
                }
                json regional_props = canonicalProps(spec.jurisdiction, spec.path, props, joined);
                features.push_back({
                    {"type", "Feature"},
                    {"id", regional_props["regional_parcel_id"]},
                    {"properties", std::move(regional_props)},
                    {"geometry", feature["geometry"]}
                });
            }
        }

        json fields = json::array({"jurisdiction", "source_file", "regional_parcel_id"});
        for (const auto& [field, _] : kFieldAliases) fields.push_back(field);
        json out = {
            {"type", "FeatureCollection"},
            {"name", "regional_parcels"},
            {"metadata", {
                {"schema_version", 1},
                {"generated_by", "worldsim_regional_parcel_builder"},
                {"source_counts", counts},
                {"canonical_fields", fields}
            }},
            {"features", std::move(features)}
        };

        fs::create_directories(output.parent_path());
        std::ofstream out_file(output);
        if (!out_file) throw std::runtime_error("failed to open output " + output.string());
        out_file << out.dump();
        std::cout << "wrote " << output << " with " << out["features"].size() << " features\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
