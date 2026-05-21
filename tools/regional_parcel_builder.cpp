#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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

constexpr char kCanonicalMagic[8] = {'W', 'S', '3', 'C', 'A', 'N', '1', '\0'};
constexpr uint32_t kCanonicalVersion = 2;
constexpr size_t kCanonicalSignatureBytes = 256;

fs::path tempOutputPathFor(const fs::path& path) {
    return fs::path(path.string() + ".tmp");
}

void replaceAtomically(const fs::path& tmp_path, const fs::path& final_path) {
    std::error_code rename_ec;
    fs::rename(tmp_path, final_path, rename_ec);
    if (!rename_ec) return;
    std::error_code remove_ec;
    fs::remove(final_path, remove_ec);
    rename_ec.clear();
    fs::rename(tmp_path, final_path, rename_ec);
    if (rename_ec) {
        fs::remove(tmp_path, remove_ec);
        throw std::runtime_error("failed to rename " + tmp_path.string() + " to " + final_path.string());
    }
}

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

bool writeExact(std::ostream& out, const void* src, size_t n) {
    out.write(static_cast<const char*>(src), static_cast<std::streamsize>(n));
    return bool(out);
}

bool readExact(std::istream& in, void* dst, size_t n) {
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    return bool(in);
}

bool readU32(std::istream& in, uint32_t& out) {
    uint8_t b[4];
    if (!readExact(in, b, sizeof(b))) return false;
    out = uint32_t(b[0]) |
          (uint32_t(b[1]) << 8) |
          (uint32_t(b[2]) << 16) |
          (uint32_t(b[3]) << 24);
    return true;
}

bool readU64(std::istream& in, uint64_t& out) {
    uint8_t b[8];
    if (!readExact(in, b, sizeof(b))) return false;
    out = uint64_t(b[0]) |
          (uint64_t(b[1]) << 8) |
          (uint64_t(b[2]) << 16) |
          (uint64_t(b[3]) << 24) |
          (uint64_t(b[4]) << 32) |
          (uint64_t(b[5]) << 40) |
          (uint64_t(b[6]) << 48) |
          (uint64_t(b[7]) << 56);
    return true;
}

bool readFloat(std::istream& in, float& out) {
    uint32_t bits = 0;
    if (!readU32(in, bits)) return false;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

bool readString(std::istream& in, std::string& out) {
    uint32_t len = 0;
    if (!readU32(in, len)) return false;
    out.assign(len, '\0');
    return len == 0 || readExact(in, out.data(), len);
}

bool writeU32(std::ostream& out, uint32_t v) {
    const uint8_t b[4] = {
        static_cast<uint8_t>(v & 0xffu),
        static_cast<uint8_t>((v >> 8) & 0xffu),
        static_cast<uint8_t>((v >> 16) & 0xffu),
        static_cast<uint8_t>((v >> 24) & 0xffu),
    };
    return writeExact(out, b, sizeof(b));
}

bool writeU64(std::ostream& out, uint64_t v) {
    const uint8_t b[8] = {
        static_cast<uint8_t>(v & 0xffu),
        static_cast<uint8_t>((v >> 8) & 0xffu),
        static_cast<uint8_t>((v >> 16) & 0xffu),
        static_cast<uint8_t>((v >> 24) & 0xffu),
        static_cast<uint8_t>((v >> 32) & 0xffu),
        static_cast<uint8_t>((v >> 40) & 0xffu),
        static_cast<uint8_t>((v >> 48) & 0xffu),
        static_cast<uint8_t>((v >> 56) & 0xffu),
    };
    return writeExact(out, b, sizeof(b));
}

bool writeFloat(std::ostream& out, float v) {
    uint32_t bits = 0;
    static_assert(sizeof(float) == sizeof(uint32_t));
    std::memcpy(&bits, &v, sizeof(bits));
    return writeU32(out, bits);
}

bool writeString(std::ostream& out, const std::string& s) {
    if (s.size() > UINT32_MAX) return false;
    return writeU32(out, static_cast<uint32_t>(s.size())) &&
           (s.empty() || writeExact(out, s.data(), s.size()));
}

std::string propertyValueString(const json& v) {
    if (v.is_null()) return "";
    if (v.is_string()) return v.get<std::string>();
    return v.dump();
}

std::string fileSignatureForPath(const fs::path& p) {
    std::error_code size_ec;
    auto sz = fs::file_size(p, size_ec);
    if (size_ec) return "missing:" + p.filename().string();
    std::error_code time_ec;
    auto wt = fs::last_write_time(p, time_ec);
    if (time_ec) return std::to_string((unsigned long long)sz) + "_mtime_unavailable";
    const auto ticks = wt.time_since_epoch().count();
    return std::to_string((unsigned long long)sz) + "_" + std::to_string((long long)ticks);
}

std::string buildInputSignature(
    const std::vector<SourceSpec>& inputs,
    const std::vector<SourceSpec>& property_inputs) {
    std::string sig = "regional_parcels_v1";
    auto append_specs = [&](const char* kind, const std::vector<SourceSpec>& specs) {
        for (const auto& spec : specs) {
            sig += "|";
            sig += kind;
            sig += ":";
            sig += spec.jurisdiction;
            sig += ":";
            sig += fileSignatureForPath(spec.path);
        }
    };
    append_specs("geom", inputs);
    append_specs("prop", property_inputs);
    return sig;
}

template <class Fn>
void forEachHydratedGeom(const json& geom, Fn&& fn) {
    if (!geom.contains("type") || !geom.contains("coordinates")) return;
    const std::string t = geom["type"].get<std::string>();
    auto emit_polygon = [&](const json& poly_coords) {
        std::vector<std::vector<std::pair<float, float>>> rings;
        rings.reserve(poly_coords.size());
        for (const auto& ring_json : poly_coords) {
            std::vector<std::pair<float, float>> ring;
            ring.reserve(ring_json.size());
            for (const auto& p : ring_json) {
                if (!p.is_array() || p.size() < 2) continue;
                ring.push_back({p[0].get<float>(), p[1].get<float>()});
            }
            if (ring.size() >= 3) rings.push_back(std::move(ring));
        }
        if (!rings.empty()) fn(rings);
    };

    if (t == "Polygon") {
        emit_polygon(geom["coordinates"]);
    } else if (t == "MultiPolygon") {
        for (const auto& poly : geom["coordinates"]) emit_polygon(poly);
    }
}

struct CanonicalBinaryWriter {
    fs::path path;
    std::ofstream out;
    uint64_t feature_count = 0;

    explicit CanonicalBinaryWriter(const fs::path& p) : path(p), out(p, std::ios::binary | std::ios::trunc) {}

    bool begin() {
        if (!out) return false;
        const uint32_t endian = 0x01020304u;
        const uint64_t zero_count = 0;
        const std::array<char, kCanonicalSignatureBytes> zero_sig{};
        return writeExact(out, kCanonicalMagic, sizeof(kCanonicalMagic)) &&
               writeU32(out, kCanonicalVersion) &&
               writeU32(out, endian) &&
               writeU64(out, zero_count) &&
               writeExact(out, zero_sig.data(), zero_sig.size());
    }

    bool appendFeature(const json& geom, const json& props) {
        bool ok = true;
        forEachHydratedGeom(geom, [&](const std::vector<std::vector<std::pair<float, float>>>& rings) {
            if (!ok) return;
            float min_lon = 0.0f, min_lat = 0.0f, max_lon = 0.0f, max_lat = 0.0f;
            bool has = false;
            for (const auto& ring : rings) {
                for (const auto& [lon, lat] : ring) {
                    if (!has) {
                        min_lon = max_lon = lon;
                        min_lat = max_lat = lat;
                        has = true;
                    } else {
                        min_lon = std::min(min_lon, lon);
                        min_lat = std::min(min_lat, lat);
                        max_lon = std::max(max_lon, lon);
                        max_lat = std::max(max_lat, lat);
                    }
                }
            }
            if (!has) return;
            ok = writeFloat(out, min_lon) &&
                 writeFloat(out, min_lat) &&
                 writeFloat(out, max_lon) &&
                 writeFloat(out, max_lat) &&
                 writeU32(out, static_cast<uint32_t>(rings.size()));
            for (const auto& ring : rings) {
                ok = ok && writeU32(out, static_cast<uint32_t>(ring.size()));
                for (const auto& [lon, lat] : ring) {
                    ok = ok && writeFloat(out, lon) && writeFloat(out, lat);
                }
            }
            ok = ok && writeU32(out, 0u);
            const uint32_t property_count = props.is_object() ? static_cast<uint32_t>(props.size()) : 0u;
            ok = ok && writeU32(out, property_count);
            if (props.is_object()) {
                for (auto it = props.begin(); ok && it != props.end(); ++it) {
                    ok = writeString(out, it.key()) && writeString(out, propertyValueString(it.value()));
                }
            }
            if (ok) ++feature_count;
        });
        return ok;
    }

    bool finalize(const std::string& sig) {
        if (!out) return false;
        out.flush();
        if (!out) return false;
        out.seekp(16, std::ios::beg);
        if (!writeU64(out, feature_count)) return false;
        std::array<char, kCanonicalSignatureBytes> sig_buf{};
        const size_t n = std::min(sig.size(), sig_buf.size() - 1);
        std::memcpy(sig_buf.data(), sig.data(), n);
        if (!writeExact(out, sig_buf.data(), sig_buf.size())) return false;
        out.flush();
        return bool(out);
    }
};

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

json geometryFromCanonicalFeature(
    const std::vector<std::vector<std::pair<float, float>>>& rings,
    const std::vector<std::vector<std::pair<float, float>>>& paths,
    float min_lon,
    float min_lat) {
    if (!rings.empty()) {
        json coords = json::array();
        for (const auto& ring : rings) {
            json ring_json = json::array();
            for (const auto& [lon, lat] : ring) ring_json.push_back({lon, lat});
            coords.push_back(std::move(ring_json));
        }
        return json{{"type", "Polygon"}, {"coordinates", std::move(coords)}};
    }
    if (!paths.empty()) {
        if (paths.size() == 1) {
            json coords = json::array();
            for (const auto& [lon, lat] : paths.front()) coords.push_back({lon, lat});
            return json{{"type", "LineString"}, {"coordinates", std::move(coords)}};
        }
        json coords = json::array();
        for (const auto& path : paths) {
            json path_json = json::array();
            for (const auto& [lon, lat] : path) path_json.push_back({lon, lat});
            coords.push_back(std::move(path_json));
        }
        return json{{"type", "MultiLineString"}, {"coordinates", std::move(coords)}};
    }
    return json{{"type", "Point"}, {"coordinates", {min_lon, min_lat}}};
}

json readCanonicalFeatureCollection(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("failed to open " + path.string());
    std::array<char, 8> magic{};
    if (!readExact(in, magic.data(), magic.size()) ||
        std::memcmp(magic.data(), kCanonicalMagic, sizeof(kCanonicalMagic)) != 0) {
        throw std::runtime_error(path.string() + " is not a canonical layer binary");
    }
    uint32_t version = 0;
    uint32_t endian = 0;
    uint64_t feature_count = 0;
    std::array<char, kCanonicalSignatureBytes> sig{};
    if (!readU32(in, version) || !readU32(in, endian) || !readU64(in, feature_count) ||
        !readExact(in, sig.data(), sig.size())) {
        throw std::runtime_error("failed to read canonical header from " + path.string());
    }
    if (version != kCanonicalVersion || endian != 0x01020304u) {
        throw std::runtime_error("unsupported canonical binary header in " + path.string());
    }
    json features = json::array();
    for (uint64_t i = 0; i < feature_count; ++i) {
        float min_lon = 0.0f, min_lat = 0.0f, max_lon = 0.0f, max_lat = 0.0f;
        if (!readFloat(in, min_lon) || !readFloat(in, min_lat) ||
            !readFloat(in, max_lon) || !readFloat(in, max_lat)) {
            throw std::runtime_error("failed to read canonical extents from " + path.string());
        }
        uint32_t ring_count = 0;
        if (!readU32(in, ring_count)) throw std::runtime_error("failed to read ring count from " + path.string());
        std::vector<std::vector<std::pair<float, float>>> rings;
        rings.reserve(ring_count);
        for (uint32_t ri = 0; ri < ring_count; ++ri) {
            uint32_t point_count = 0;
            if (!readU32(in, point_count)) throw std::runtime_error("failed to read ring point count from " + path.string());
            std::vector<std::pair<float, float>> ring;
            ring.reserve(point_count);
            for (uint32_t pi = 0; pi < point_count; ++pi) {
                float lon = 0.0f, lat = 0.0f;
                if (!readFloat(in, lon) || !readFloat(in, lat)) {
                    throw std::runtime_error("failed to read ring coordinate from " + path.string());
                }
                ring.push_back({lon, lat});
            }
            rings.push_back(std::move(ring));
        }
        uint32_t path_count = 0;
        if (!readU32(in, path_count)) throw std::runtime_error("failed to read path count from " + path.string());
        std::vector<std::vector<std::pair<float, float>>> paths;
        paths.reserve(path_count);
        for (uint32_t pi = 0; pi < path_count; ++pi) {
            uint32_t point_count = 0;
            if (!readU32(in, point_count)) throw std::runtime_error("failed to read path point count from " + path.string());
            std::vector<std::pair<float, float>> path_points;
            path_points.reserve(point_count);
            for (uint32_t vi = 0; vi < point_count; ++vi) {
                float lon = 0.0f, lat = 0.0f;
                if (!readFloat(in, lon) || !readFloat(in, lat)) {
                    throw std::runtime_error("failed to read path coordinate from " + path.string());
                }
                path_points.push_back({lon, lat});
            }
            paths.push_back(std::move(path_points));
        }
        uint32_t property_count = 0;
        if (!readU32(in, property_count)) throw std::runtime_error("failed to read property count from " + path.string());
        json props = json::object();
        for (uint32_t pi = 0; pi < property_count; ++pi) {
            std::string key;
            std::string value;
            if (!readString(in, key) || !readString(in, value)) {
                throw std::runtime_error("failed to read property from " + path.string());
            }
            props[key] = value;
        }
        features.push_back({
            {"type", "Feature"},
            {"properties", std::move(props)},
            {"geometry", geometryFromCanonicalFeature(rings, paths, min_lon, min_lat)}
        });
    }
    return json{{"type", "FeatureCollection"}, {"features", std::move(features)}};
}

json readSourceCollection(const fs::path& path) {
    return path.filename().string().ends_with(".canonical.bin")
        ? readCanonicalFeatureCollection(path)
        : readFeatureCollection(path);
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
        throw std::runtime_error("source specs must be Jurisdiction:path: " + raw);
    }
    return {raw.substr(0, pos), fs::path(raw.substr(pos + 1))};
}

void usage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " --input Jurisdiction:path.geojson.canonical.bin [--property-input Jurisdiction:path.geojson.canonical.bin]"
              << " --output data/world/earth/nation_state/us/state_region/md/layers/regional_parcels.geojson.canonical.bin\n"
              << "Writes only the canonical binary output.\n";
}
}

int main(int argc, char** argv) {
    try {
        std::vector<SourceSpec> inputs;
        std::vector<SourceSpec> property_inputs;
        fs::path output = "data/world/earth/nation_state/us/state_region/md/layers/regional_parcels.geojson.canonical.bin";
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
            json fc = readSourceCollection(spec.path);
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

        fs::create_directories(output.parent_path());
        const fs::path canonical_output =
            output.filename().string().ends_with(".canonical.bin")
                ? output
                : fs::path(output.string() + ".canonical.bin");
        const fs::path canonical_tmp = tempOutputPathFor(canonical_output);
        std::error_code cleanup_ec;
        fs::remove(canonical_tmp, cleanup_ec);
        CanonicalBinaryWriter canonical_writer(canonical_tmp);
        if (!canonical_writer.begin()) {
            throw std::runtime_error("failed to open canonical binary output " + canonical_output.string());
        }

        size_t feature_count = 0;
        for (const auto& spec : inputs) {
            json fc = readSourceCollection(spec.path);
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
                json out_feature = {
                    {"type", "Feature"},
                    {"id", regional_props["regional_parcel_id"]},
                    {"properties", std::move(regional_props)},
                    {"geometry", feature["geometry"]}
                };
                if (!canonical_writer.appendFeature(feature["geometry"], out_feature["properties"])) {
                    throw std::runtime_error("failed to append canonical binary feature");
                }
                ++feature_count;
            }
        }
        const std::string output_sig = buildInputSignature(inputs, property_inputs);
        if (!canonical_writer.finalize(output_sig)) {
            throw std::runtime_error("failed to finalize canonical binary output " + canonical_output.string());
        }
        replaceAtomically(canonical_tmp, canonical_output);
        if (output != canonical_output) fs::remove(output, cleanup_ec);
        std::cout << "wrote " << canonical_output << " with " << feature_count << " features\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
