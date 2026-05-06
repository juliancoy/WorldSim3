#include "zoning.h"

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

