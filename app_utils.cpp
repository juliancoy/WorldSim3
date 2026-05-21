#include "app_utils.h"

#include "feature_props.h"
#include "layer_state_io.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <numbers>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
std::string safeProvenanceComponent(const std::string& value, const std::string& fallback = {}) {
    return value.empty() ? fallback : value;
}

fs::path provenanceHierarchyRoot(
    const fs::path& root,
    const char* top_level,
    const std::string& world,
    const std::string& nation_state,
    const std::string& state_region,
    const std::string& county_city) {
    fs::path out = root / top_level / "world" / safeProvenanceComponent(world, "earth");
    if (!nation_state.empty()) out /= fs::path("nation_state") / nation_state;
    if (!state_region.empty()) out /= fs::path("state_region") / state_region;
    if (!county_city.empty()) out /= fs::path("county_city") / county_city;
    return out;
}

const LayerDef* findManifestLayerByFile(const fs::path& root, const std::string& file, std::vector<LayerDef>& scratch) {
    scratch = loadManifest(root);
    for (auto& layer : scratch) {
        if (layerMatchesIdentifier(layer, file)) return &layer;
    }
    return nullptr;
}

bool hasManifestName(const fs::path& path) {
    const std::string name = path.filename().string();
    return name.starts_with("layers_manifest") && name.ends_with(".json");
}

const LayerDef* findManifestLayerByFileIncludingNonRuntime(
    const fs::path& root,
    const std::string& file,
    std::vector<LayerDef>& scratch) {
    std::error_code ec;
    const fs::path manifest_root = root / "sources" / "world";
    if (!fs::exists(manifest_root, ec) || ec) return nullptr;
    for (fs::recursive_directory_iterator it(manifest_root, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file() || !hasManifestName(it->path())) continue;
        std::ifstream in(it->path());
        if (!in) continue;
        json arr;
        try {
            in >> arr;
        } catch (...) {
            continue;
        }
        if (!arr.is_array()) continue;
        for (const auto& item : arr) {
            if (!item.is_object()) continue;
            const std::string item_file = item.value("file", std::string());
            const std::string item_logical_id = item.value("id", defaultLayerLogicalIdForFile(item_file));
            if (file != item_file && file != item_logical_id) continue;
            LayerDef layer;
            layer.file = item_file;
            layer.logical_id = item_logical_id;
            if (item.contains("provenance") && item["provenance"].is_object()) {
                const auto& provenance = item["provenance"];
                layer.provenance_world = provenance.value("world", std::string());
                layer.provenance_nation_state = provenance.value("nation_state", std::string());
                layer.provenance_state_region = provenance.value("state_region", std::string());
                layer.provenance_county_city = provenance.value("county_city", std::string());
            }
            scratch.clear();
            scratch.push_back(std::move(layer));
            return &scratch.back();
        }
    }
    return nullptr;
}

fs::path wellKnownStoredLayerPathForFile(const fs::path& root, const std::string& file) {
    if (file == "parcel.geojson") {
        return root / "data" / "world" / "earth" / "nation_state" / "us" / "state_region" / "md" / "county_city" /
               "baltimore_city" / "layers" / file;
    }
    return {};
}
}

const char* categoryToString(LayerDef::Category c) {
    switch (c) {
        case LayerDef::Category::Housing: return "housing";
        case LayerDef::Category::PublicHealth: return "public_health";
        case LayerDef::Category::Infrastructure: return "infrastructure";
        case LayerDef::Category::Zoning: return "zoning";
        case LayerDef::Category::Safety: return "safety";
    }
    return "unknown";
}

std::pair<int, int> deg2num(double lat_deg, double lon_deg, int zoom) {
    double lat_rad = lat_deg * std::numbers::pi / 180.0;
    double n = std::pow(2.0, zoom);
    int xtile = (int)((lon_deg + 180.0) / 360.0 * n);
    int ytile = (int)((1.0 - std::asinh(std::tan(lat_rad)) / std::numbers::pi) / 2.0 * n);
    return {xtile, ytile};
}

std::filesystem::path resolveAppRoot(const fs::path& start, const char* argv0) {
    auto has_manifest = [](const fs::path& p) {
        std::error_code ec;
        if (!fs::exists(p / "sources" / "world", ec) || ec) return false;
        for (fs::recursive_directory_iterator it(p / "sources" / "world", ec), end; it != end && !ec; it.increment(ec)) {
            if (!it->is_regular_file()) continue;
            const std::string name = it->path().filename().string();
            if (name.starts_with("layers_manifest") && name.ends_with(".json")) return true;
        }
        return false;
    };
    auto climb = [&](fs::path p) -> fs::path {
        std::error_code ec;
        p = fs::weakly_canonical(p, ec);
        if (ec) p = fs::absolute(p);
        for (;;) {
            if (has_manifest(p)) return p;
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
        return {};
    };

    if (fs::path found = climb(start); !found.empty()) return found;
    if (argv0 && *argv0) {
        fs::path exe_path(argv0);
        if (exe_path.has_parent_path()) {
            if (fs::path found = climb(exe_path.parent_path()); !found.empty()) return found;
        }
    }
    return start;
}

void setBootstrapStatus(BootstrapProgress& bp, const std::string& s) {
    std::lock_guard<std::mutex> lk(bp.msg_mutex);
    bp.status = s;
}

std::string readTextFile(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void collectTodoWork(const std::string& todo_text, std::vector<std::string>& past, std::vector<std::string>& future) {
    std::istringstream in(todo_text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("- [x]", 0) == 0 || line.rfind("- [X]", 0) == 0) {
            past.push_back(line.substr(5));
            continue;
        }
        if (line.rfind("- ", 0) == 0) {
            future.push_back(line.substr(2));
            continue;
        }
    }
}

std::string toLowerAscii(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

std::string normalizeGeographyToken(const std::string& s) {
    return toLowerAscii(trimDisplayValue(s));
}

bool containsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    const size_t limit = haystack.size() - needle.size();
    auto lower_ascii = [](char ch) -> char {
        return (ch >= 'A' && ch <= 'Z') ? (char)(ch + ('a' - 'A')) : ch;
    };
    for (size_t i = 0; i <= limit; ++i) {
        size_t j = 0;
        for (; j < needle.size(); ++j) {
            if (lower_ascii(haystack[i + j]) != lower_ascii(needle[j])) break;
        }
        if (j == needle.size()) return true;
    }
    return false;
}

bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    return containsCaseInsensitive(std::string_view(haystack), std::string_view(needle));
}

bool isLikelyCrimePointLayer(const LayerDef& layer) {
    return containsCaseInsensitive(layer.name, "crime") ||
           containsCaseInsensitive(layer.subcategory, "crime") ||
           containsCaseInsensitive(layer.file, "crime_nibrs");
}

namespace {

std::string firstCrimeProp(const LayerDef::FeatureRecord& fg, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        std::string v = getPropertyValue(fg, k);
        if (!v.empty()) return v;
    }
    return {};
}

std::string normalizedCrimeDescriptor(const LayerDef::FeatureRecord& fg) {
    const std::string desc = toLowerAscii(firstCrimeProp(fg, {"Description", "description", "OFFENSE", "UCRDescription"}));
    const std::string code = toLowerAscii(firstCrimeProp(fg, {"CrimeCode", "UCR_CODE", "UCRCode"}));
    if (desc.empty()) return code;
    if (code.empty()) return desc;
    return desc + " " + code;
}

bool crimeDescriptorHas(const std::string& descriptor, const char* needle) {
    return !descriptor.empty() && descriptor.find(needle) != std::string::npos;
}

} // namespace

uint32_t crimePointGlyphCode(const LayerDef::FeatureRecord& fg) {
    const std::string descriptor = normalizedCrimeDescriptor(fg);
    if (crimeDescriptorHas(descriptor, "shooting")) return 5; // Cross
    if (crimeDescriptorHas(descriptor, "homicide") || crimeDescriptorHas(descriptor, "murder")) return 4; // Plus
    if (crimeDescriptorHas(descriptor, "robbery")) return 2; // Diamond
    if (crimeDescriptorHas(descriptor, "assault")) return 3; // Triangle
    if (crimeDescriptorHas(descriptor, "burglary")) return 1; // Square
    if (crimeDescriptorHas(descriptor, "motor vehicle theft") ||
        crimeDescriptorHas(descriptor, "auto theft") ||
        crimeDescriptorHas(descriptor, "vehicle theft")) return 6; // Droplet
    if (crimeDescriptorHas(descriptor, "drug") || crimeDescriptorHas(descriptor, "narcotic")) return 1; // Square
    if (crimeDescriptorHas(descriptor, "larceny") || crimeDescriptorHas(descriptor, "theft")) return 0; // Circle
    return 0; // Circle
}

const char* crimePointTypeLabel(const LayerDef::FeatureRecord& fg) {
    const std::string descriptor = normalizedCrimeDescriptor(fg);
    if (crimeDescriptorHas(descriptor, "shooting")) return "Shooting";
    if (crimeDescriptorHas(descriptor, "homicide") || crimeDescriptorHas(descriptor, "murder")) return "Homicide";
    if (crimeDescriptorHas(descriptor, "robbery")) return "Robbery";
    if (crimeDescriptorHas(descriptor, "assault")) return "Assault";
    if (crimeDescriptorHas(descriptor, "burglary")) return "Burglary";
    if (crimeDescriptorHas(descriptor, "motor vehicle theft") ||
        crimeDescriptorHas(descriptor, "auto theft") ||
        crimeDescriptorHas(descriptor, "vehicle theft")) return "Auto theft";
    if (crimeDescriptorHas(descriptor, "drug") || crimeDescriptorHas(descriptor, "narcotic")) return "Drug offense";
    if (crimeDescriptorHas(descriptor, "larceny") || crimeDescriptorHas(descriptor, "theft")) return "Theft";
    return "Crime incident";
}

std::string normalizeFuzzySearchText(const std::string& s) {
    std::string cleaned;
    cleaned.reserve(s.size());
    for (unsigned char ch : s) {
        if (std::isalnum(ch)) cleaned.push_back((char)std::tolower(ch));
        else cleaned.push_back(' ');
    }
    std::istringstream in(cleaned);
    std::ostringstream out;
    std::string token;
    bool first = true;
    while (in >> token) {
        if (!first) out << ' ';
        out << token;
        first = false;
    }
    return out.str();
}

int fuzzyTextScore(const std::string& text, const std::string& query) {
    const std::string q = trimDisplayValue(query);
    if (q.empty()) return 1;
    const std::string text_norm = normalizeFuzzySearchText(text);
    const std::string query_norm = normalizeFuzzySearchText(q);
    if (text_norm.empty() || query_norm.empty()) return 0;
    if (text_norm == query_norm) return 120;
    if (text_norm.rfind(query_norm, 0) == 0) return 105;
    if (text_norm.find(query_norm) != std::string::npos) return 90;

    std::istringstream tokens(query_norm);
    std::string token;
    int token_count = 0;
    int matched_tokens = 0;
    int score = 0;
    while (tokens >> token) {
        token_count++;
        const size_t pos = text_norm.find(token);
        if (pos != std::string::npos) {
            matched_tokens++;
            score += pos == 0 ? 28 : 20;
            continue;
        }
        if (token.size() >= 4) {
            bool approximate = false;
            std::istringstream hay_tokens(text_norm);
            std::string ht;
            while (hay_tokens >> ht) {
                if (ht.size() < 4) continue;
                const size_t prefix = std::min<size_t>(4, std::min(ht.size(), token.size()));
                if (ht.compare(0, prefix, token, 0, prefix) == 0) {
                    approximate = true;
                    break;
                }
            }
            if (approximate) {
                matched_tokens++;
                score += 12;
            }
        }
    }
    if (token_count == 0 || matched_tokens == 0) return 0;
    if (matched_tokens == token_count) return 35 + score;
    return matched_tokens >= 2 ? 15 + score : 0;
}

bool fuzzyTextMatches(const std::string& text, const std::string& query, int min_score) {
    return fuzzyTextScore(text, query) >= min_score;
}

std::string normalizeAddressSearchText(const std::string& s) {
    static const std::unordered_map<std::string, std::string> kCanonical{
        {"avenue", "ave"}, {"av", "ave"},
        {"boulevard", "blvd"}, {"boul", "blvd"},
        {"circle", "cir"}, {"court", "ct"}, {"drive", "dr"},
        {"highway", "hwy"}, {"lane", "ln"}, {"parkway", "pkwy"},
        {"place", "pl"}, {"road", "rd"}, {"square", "sq"},
        {"street", "st"}, {"terrace", "ter"}, {"trail", "trl"},
        {"north", "n"}, {"south", "s"}, {"east", "e"}, {"west", "w"},
        {"northeast", "ne"}, {"northwest", "nw"}, {"southeast", "se"}, {"southwest", "sw"},
        {"apartment", "apt"}, {"unit", "apt"}, {"suite", "ste"},
    };

    std::string cleaned;
    cleaned.reserve(s.size());
    for (unsigned char ch : s) {
        if (std::isalnum(ch)) cleaned.push_back((char)std::tolower(ch));
        else cleaned.push_back(' ');
    }

    std::istringstream in(cleaned);
    std::ostringstream out;
    std::string token;
    bool first = true;
    while (in >> token) {
        if (token.size() > 2) {
            const std::string suffix = token.substr(token.size() - 2);
            const bool ordinal_suffix = suffix == "st" || suffix == "nd" || suffix == "rd" || suffix == "th";
            if (ordinal_suffix && std::all_of(token.begin(), token.end() - 2, [](unsigned char ch) {
                    return std::isdigit(ch) != 0;
                })) {
                token.resize(token.size() - 2);
            }
        }
        auto it = kCanonical.find(token);
        if (it != kCanonical.end()) token = it->second;
        if (!first) out << ' ';
        out << token;
        first = false;
    }
    return out.str();
}

int addressSearchScore(const std::string& address, const std::string& query) {
    const std::string q = trimDisplayValue(query);
    if (q.empty()) return 0;
    const std::string address_lc = toLowerAscii(address);
    const std::string q_lc = toLowerAscii(q);
    if (address_lc == q_lc) return 120;

    const std::string normalized_address = normalizeAddressSearchText(address);
    const std::string normalized_query = normalizeAddressSearchText(q);
    if (normalized_query.empty()) return 0;
    if (normalized_address == normalized_query) return 110;
    if (normalized_address.rfind(normalized_query, 0) == 0) return 100;
    if (normalized_address.find(normalized_query) != std::string::npos) return 90;

    std::istringstream tokens(normalized_query);
    std::string token;
    int token_count = 0;
    while (tokens >> token) {
        token_count++;
        if (normalized_address.find(token) == std::string::npos) return 0;
    }
    return token_count > 0 ? 40 + token_count : 0;
}

bool addressMatchesSearch(const std::string& address, const std::string& query) {
    if (trimDisplayValue(query).empty()) return true;
    if (containsCaseInsensitive(address, query)) return true;
    return addressSearchScore(address, query) > 0;
}

int extractYearMaybe(const std::string& s) {
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

double parseNumericField(const std::string& s) {
    std::string cleaned;
    cleaned.reserve(s.size());
    for (char ch : s) {
        if (std::isdigit((unsigned char)ch) || ch == '.' || ch == '-' || ch == '+') cleaned.push_back(ch);
    }
    if (cleaned.empty() || cleaned == "-" || cleaned == "+") return 0.0;
    try {
        return std::stod(cleaned);
    } catch (...) {
        return 0.0;
    }
}

std::string formatUsNumber(double value, int decimals) {
    decimals = std::max(0, decimals);
    const bool negative = value < 0.0;
    const double abs_value = std::abs(value);
    std::ostringstream raw;
    raw << std::fixed << std::setprecision(decimals) << abs_value;
    std::string s = raw.str();
    const size_t dot = s.find('.');
    const std::string whole = dot == std::string::npos ? s : s.substr(0, dot);
    const std::string frac = dot == std::string::npos ? std::string() : s.substr(dot);

    std::string grouped;
    grouped.reserve(whole.size() + whole.size() / 3 + frac.size() + 1);
    if (negative) grouped.push_back('-');
    const size_t first_group = whole.size() % 3;
    size_t i = 0;
    if (first_group != 0) {
        grouped.append(whole, 0, first_group);
        i = first_group;
        if (i < whole.size()) grouped.push_back(',');
    }
    for (; i < whole.size(); i += 3) {
        grouped.append(whole, i, 3);
        if (i + 3 < whole.size()) grouped.push_back(',');
    }
    grouped += frac;
    return grouped;
}

std::string formatUsd(double value, int decimals) {
    return "$" + formatUsNumber(value, decimals);
}

std::filesystem::path provenanceStoredLayerPath(const fs::path& root, const LayerDef& layer) {
    return provenanceHierarchyRoot(
               root,
               "data",
               layer.provenance_world,
               layer.provenance_nation_state,
               layer.provenance_state_region,
               layer.provenance_county_city) /
           "layers" / layer.file;
}

std::filesystem::path provenanceSourceArtifactPath(const fs::path& root, const LayerDef& layer, const std::string& artifact_name) {
    return provenanceHierarchyRoot(
               root,
               "sources",
               layer.provenance_world,
               layer.provenance_nation_state,
               layer.provenance_state_region,
               layer.provenance_county_city) /
           "layers" / artifact_name;
}

std::filesystem::path resolveStoredLayerPath(const fs::path& root, const LayerDef& layer) {
    const fs::path provenance_path = provenanceStoredLayerPath(root, layer);
    std::error_code ec;
    if (fs::exists(provenance_path, ec) && !ec) return provenance_path;
    const fs::path legacy_path = root / "data" / "layers" / layer.file;
    return legacy_path;
}

std::filesystem::path resolveStoredLayerPathForFile(const fs::path& root, const std::string& file) {
    std::vector<LayerDef> scratch;
    if (const LayerDef* layer = findManifestLayerByFile(root, file, scratch)) {
        const fs::path provenance_path = provenanceStoredLayerPath(root, *layer);
        std::error_code ec;
        if (fs::exists(provenance_path, ec) && !ec) return provenance_path;
        return root / "data" / "layers" / file;
    }
    if (const LayerDef* layer = findManifestLayerByFileIncludingNonRuntime(root, file, scratch)) {
        const fs::path provenance_path = provenanceStoredLayerPath(root, *layer);
        std::error_code ec;
        if (fs::exists(provenance_path, ec) && !ec) return provenance_path;
    }
    if (const fs::path known_path = wellKnownStoredLayerPathForFile(root, file); !known_path.empty()) {
        std::error_code ec;
        if (fs::exists(known_path, ec) && !ec) return known_path;
    }
    return root / "data" / "layers" / file;
}

std::filesystem::path canonicalLayerPathForFile(const fs::path& root, const std::string& file) {
    const fs::path layer_path = resolveStoredLayerPathForFile(root, file);
    return layer_path.parent_path() / (layerArtifactBasenameForFile(file) + ".canonical.bin");
}

bool layerRuntimeSourceMaterializedForFile(const fs::path& root, const std::string& file) {
    std::error_code ec;
    return fs::exists(canonicalLayerPathForFile(root, file), ec) && !ec;
}

bool layerRuntimeSourceMaterialized(const fs::path& root, const LayerDef& layer) {
    std::error_code ec;
    const fs::path layer_path = resolveStoredLayerPath(root, layer);
    const fs::path canonical_path =
        layer_path.parent_path() / (layerArtifactBasenameForFile(layer.file) + ".canonical.bin");
    return fs::exists(canonical_path, ec) && !ec;
}

std::string trimDisplayValue(std::string s) {
    auto is_ws = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!s.empty() && is_ws((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws((unsigned char)s.back())) s.pop_back();
    return s;
}

std::string defaultLayerLogicalIdForFile(const std::string& file) {
    if (file.ends_with(".geojson")) return file.substr(0, file.size() - std::strlen(".geojson"));
    return file;
}

std::string layerLogicalId(const LayerDef& layer) {
    return layer.logical_id.empty() ? defaultLayerLogicalIdForFile(layer.file) : layer.logical_id;
}

bool layerMatchesIdentifier(const LayerDef& layer, std::string_view key) {
    return layer.file == key || layerLogicalId(layer) == key;
}

std::string layerArtifactBasenameForFile(const std::string& file) {
    return file;
}

void invalidateLayerGeometryUsageCache(LayerDef& layer) {
    layer.geometry_usage_cache_valid = false;
}

void refreshLayerGeometryUsageCache(LayerDef& layer) {
    bool uses_point_geometry = false;
    bool uses_polyline_geometry = false;

    if (layer.scale == "point" ||
        layer.duckdb_role == "point_event" ||
        (!layer.import_lon_field.empty() && !layer.import_lat_field.empty()) ||
        containsCaseInsensitive(layer.import_type, "point")) {
        uses_point_geometry = true;
    }

    if (containsCaseInsensitive(layer.scale, "line") ||
        containsCaseInsensitive(layer.import_type, "line")) {
        uses_point_geometry = false;
        uses_polyline_geometry = true;
    } else if (!layer.features.empty()) {
        uses_point_geometry = std::all_of(
            layer.features.begin(),
            layer.features.end(),
            [](const LayerDef::FeatureRecord& fg) {
                return fg.rings.empty() && fg.paths.empty();
            });
        uses_polyline_geometry = std::any_of(
            layer.features.begin(),
            layer.features.end(),
            [](const LayerDef::FeatureRecord& fg) {
                return !fg.paths.empty();
            });
    }

    layer.uses_point_geometry_cache = uses_point_geometry;
    layer.uses_polyline_geometry_cache = uses_polyline_geometry;
    layer.geometry_usage_cache_valid = true;
}

bool layerUsesPointGeometry(const LayerDef& layer) {
    if (!layer.geometry_usage_cache_valid) {
        refreshLayerGeometryUsageCache(const_cast<LayerDef&>(layer));
    }
    return layer.uses_point_geometry_cache;
}

bool layerUsesPolylineGeometry(const LayerDef& layer) {
    if (!layer.geometry_usage_cache_valid) {
        refreshLayerGeometryUsageCache(const_cast<LayerDef&>(layer));
    }
    return layer.uses_polyline_geometry_cache;
}

std::string firstDisplayProperty(const LayerDef::FeatureRecord& fg, std::initializer_list<const char*> keys) {
    return trimDisplayValue(getFirstPropertyValue(fg, keys));
}

std::string firstDisplayProperty(const LayerDef& layer, size_t feature_idx, std::initializer_list<const char*> keys) {
    return trimDisplayValue(getFirstPropertyValue(layer, feature_idx, keys));
}

std::string blockLotJoinKeyFromParts(const std::string& block, const std::string& lot) {
    std::string b = normalizeJoinKey(block);
    std::string l = normalizeJoinKey(lot);
    if (b.empty() || l.empty()) return "";
    return b + l;
}

std::string featureBlockLotJoinKey(const LayerDef::FeatureRecord& fg) {
    std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
    if (!bl.empty()) return bl;
    bl = normalizeJoinKey(getPropertyValue(fg, "blocklot"));
    if (!bl.empty()) return bl;
    bl = normalizeJoinKey(getPropertyValue(fg, "source_parcel_id"));
    if (!bl.empty()) return bl;
    bl = normalizeJoinKey(getPropertyValue(fg, "account_id"));
    if (!bl.empty()) return bl;
    bl = normalizeJoinKey(getPropertyValue(fg, "PIN"));
    if (!bl.empty()) return bl;
    bl = normalizeJoinKey(getPropertyValue(fg, "pin"));
    if (!bl.empty()) return bl;
    bl = blockLotJoinKeyFromParts(getPropertyValue(fg, "BLOCK"), getPropertyValue(fg, "LOT"));
    if (!bl.empty()) return bl;
    return blockLotJoinKeyFromParts(getPropertyValue(fg, "block"), getPropertyValue(fg, "lot"));
}

std::string featureBlockLotJoinKey(const LayerDef& layer, size_t feature_idx) {
    std::string bl = normalizeJoinKey(getPropertyValue(layer, feature_idx, "BLOCKLOT"));
    if (!bl.empty()) return bl;
    bl = normalizeJoinKey(getPropertyValue(layer, feature_idx, "blocklot"));
    if (!bl.empty()) return bl;
    bl = normalizeJoinKey(getPropertyValue(layer, feature_idx, "source_parcel_id"));
    if (!bl.empty()) return bl;
    bl = normalizeJoinKey(getPropertyValue(layer, feature_idx, "account_id"));
    if (!bl.empty()) return bl;
    bl = normalizeJoinKey(getPropertyValue(layer, feature_idx, "PIN"));
    if (!bl.empty()) return bl;
    bl = normalizeJoinKey(getPropertyValue(layer, feature_idx, "pin"));
    if (!bl.empty()) return bl;
    bl = blockLotJoinKeyFromParts(
        getPropertyValue(layer, feature_idx, "BLOCK"),
        getPropertyValue(layer, feature_idx, "LOT"));
    if (!bl.empty()) return bl;
    return blockLotJoinKeyFromParts(
        getPropertyValue(layer, feature_idx, "block"),
        getPropertyValue(layer, feature_idx, "lot"));
}

std::string featureStableIdForLayerFeature(const LayerDef& layer, const LayerDef::FeatureRecord& fg, size_t feature_idx) {
    auto candidate = [&](std::initializer_list<const char*> keys) {
        for (const char* key : keys) {
            std::string v = trimDisplayValue(getPropertyValue(fg, key));
            if (!v.empty()) return v;
        }
        return std::string();
    };

    std::string stable = candidate({
        "feature_id", "FEATURE_ID", "FeatureID", "globalid", "GLOBALID", "GlobalID",
        "regional_parcel_id", "source_parcel_id", "account_id", "OBJECTID_1", "OBJECTID",
        "objectid", "ID", "id", "PIN", "pin"
    });
    if (stable.empty()) stable = featureBlockLotJoinKey(fg);
    if (!stable.empty()) return normalizeJoinKey(stable);

    stable = candidate({
        "name", "Name", "NAME", "poi_name", "prmry_name", "FULLADDR", "PROPERTY_ADDRESS",
        "ADDRESS", "Address", "SITE_ADDR", "SITUSADDR"
    });
    if (!stable.empty()) return normalizeJoinKey(stable);

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(6)
       << trimDisplayValue(layer.file) << ':'
       << fg.extent.min_lon << ','
       << fg.extent.min_lat << ','
       << fg.extent.max_lon << ','
       << fg.extent.max_lat << ':'
       << feature_idx;
    return normalizeJoinKey(ss.str());
}

void openUrlInBrowser(const std::string& url) {
#if defined(_WIN32)
    std::string cmd = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
    std::string cmd = "open \"" + url + "\"";
#else
    std::string cmd = "xdg-open \"" + url + "\"";
#endif
    int rc = std::system(cmd.c_str());
    (void)rc;
}
