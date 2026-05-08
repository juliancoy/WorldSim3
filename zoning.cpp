#include "zoning.h"

#include "feature_props.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

bool parseHexColor(const std::string& hex, ImVec4& out) {
    if (hex.size() != 7 || hex[0] != '#') return false;
    auto val = [&](size_t off) -> int {
        try {
            return std::stoi(hex.substr(off, 2), nullptr, 16);
        } catch (...) {
            return -1;
        }
    };
    int r = val(1), g = val(3), b = val(5);
    if (r < 0 || g < 0 || b < 0) return false;
    out = ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    return true;
}

std::unordered_map<std::string, ZoneMetadata> loadZoneMetadata(const fs::path& root) {
    std::unordered_map<std::string, ZoneMetadata> out;
    std::ifstream in(root / "data" / "zoning_classes.json");
    if (!in) in.open(root / "zoning_classes.json");
    if (!in) return out;
    json j;
    try {
        in >> j;
    } catch (...) {
        return out;
    }
    if (!j.contains("zones") || !j["zones"].is_object()) return out;
    for (auto it = j["zones"].begin(); it != j["zones"].end(); ++it) {
        if (!it.value().is_object()) continue;
        ZoneMetadata meta;
        const auto& v = it.value();
        if (v.contains("label") && v["label"].is_string()) meta.label = v["label"].get<std::string>();
        if (v.contains("description") && v["description"].is_string()) meta.description = v["description"].get<std::string>();
        if (v.contains("color") && v["color"].is_string()) {
            meta.color_hex = v["color"].get<std::string>();
            meta.has_color = parseHexColor(meta.color_hex, meta.color);
        }
        out[it.key()] = std::move(meta);
    }
    return out;
}

namespace {
std::string trimAscii(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && is_space((unsigned char)s.back())) s.pop_back();
    return s;
}

std::string upperAscii(std::string s) {
    for (char& ch : s) ch = (char)std::toupper((unsigned char)ch);
    return s;
}

const ZoneMetadata* findZoneMetadata(
    const std::string& zone_key,
    const std::unordered_map<std::string, ZoneMetadata>& zoning_metadata) {
    auto it = zoning_metadata.find(zone_key);
    if (it != zoning_metadata.end()) return &it->second;
    const std::string upper = upperAscii(zone_key);
    it = zoning_metadata.find(upper);
    if (it != zoning_metadata.end()) return &it->second;
    return nullptr;
}

std::string firstDescriptionProperty(const LayerDef::FeatureGeom& zone) {
    const char* keys[] = {
        "Description", "description", "DESCRIPTION", "ZoningDescription", "ZONE_DESCRIPTION",
        "ZoneDescription", "ZONE_DESC", "ZONING_DESC", "Desc", "DESC"
    };
    for (const char* key : keys) {
        std::string v = trimAscii(getPropertyValue(zone, key));
        if (!v.empty()) return v;
    }
    return {};
}

std::string fallbackDescriptionForZoneCode(const std::string& raw_code) {
    const std::string code = upperAscii(trimAscii(raw_code));
    if (code.empty() || code == "UNSPECIFIED") return "Zoning district with no class description available.";

    if (code.rfind("R-", 0) == 0) return "Residential zoning district regulating housing form, density, setbacks, and compatible neighborhood uses.";
    if (code.rfind("OR-", 0) == 0) return "Office-residential zoning district allowing a mix of residential and office uses with development standards for mixed-use corridors.";
    if (code.rfind("C-", 0) == 0) return "Commercial zoning district for retail, service, office, and other business uses at an intensity set by the district.";
    if (code.rfind("EC-", 0) == 0) return "Employment center zoning district intended for job-generating commercial, office, institutional, or light industrial activity.";
    if (code.rfind("I-", 0) == 0) return "Industrial zoning district for production, distribution, repair, warehousing, and related employment uses.";
    if (code.rfind("IMU-", 0) == 0) return "Industrial mixed-use zoning district allowing industrial activity alongside compatible commercial or residential uses.";
    if (code.rfind("TOD-", 0) == 0) return "Transit-oriented development zoning district supporting compact mixed-use development near transit access.";
    if (code.rfind("PC-", 0) == 0) return "Planned center zoning district for coordinated mixed-use development under area-specific controls.";
    if (code == "OS") return "Open space zoning district for parks, recreation, conservation, and public open-space uses.";
    if (code == "BSC") return "Business and special control zoning district with area-specific business and design controls.";
    if (code == "H") return "Harbor zoning district with waterfront-oriented development and use controls.";
    if (code == "MI") return "Maritime industrial zoning district for port, marine, industrial, and water-dependent activity.";
    if (code == "OIC") return "Office-industrial campus zoning district for campus-style employment, office, research, or institutional uses.";

    return "Zoning district with use and development standards defined by the city zoning code.";
}
}

std::string zoningDescription(
    const LayerDef::FeatureGeom& zone,
    const std::unordered_map<std::string, ZoneMetadata>& zoning_metadata) {
    const std::string zone_key = zoningClassKey(zone);
    if (const ZoneMetadata* meta = findZoneMetadata(zone_key, zoning_metadata)) {
        if (!meta->description.empty()) return meta->description;
    }
    std::string feature_description = firstDescriptionProperty(zone);
    if (!feature_description.empty()) return feature_description;

    std::string description = fallbackDescriptionForZoneCode(zone_key);
    const std::string url = trimAscii(getPropertyValue(zone, "URL"));
    if (!url.empty()) description += " Source sheet: " + url;
    return description;
}
