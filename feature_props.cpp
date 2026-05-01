#include "feature_props.h"

#include <algorithm>
#include <cctype>
#include <cmath>

std::string getPropertyValue(const LayerDef::FeatureGeom& fg, const std::string& key) {
    for (const auto& kv : fg.properties) {
        if (kv.first == key) return kv.second;
    }
    return "";
}

std::string normalizeJoinKey(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        unsigned char u = (unsigned char)ch;
        if (std::isalnum(u)) out.push_back((char)std::toupper(u));
    }
    return out;
}

std::string zoningClassKey(const LayerDef::FeatureGeom& fg) {
    std::string z = getPropertyValue(fg, "Zoning");
    if (z.empty()) z = getPropertyValue(fg, "Label");
    if (z.empty()) z = getPropertyValue(fg, "ZoningLabel");
    if (z.empty()) z = getPropertyValue(fg, "ZONING");
    if (z.empty()) z = getPropertyValue(fg, "ZONE");
    if (z.empty()) z = getPropertyValue(fg, "CLASS");
    if (z.empty()) z = getPropertyValue(fg, "DISTRICT");
    if (z.empty()) return "UNSPECIFIED";
    return z;
}

std::string zoningGroupKey(const std::string& zone_key) {
    if (zone_key.empty()) return "OTHER";
    size_t end = 0;
    while (end < zone_key.size() && std::isalpha((unsigned char)zone_key[end])) end++;
    if (end == 0) return "OTHER";
    return zone_key.substr(0, end);
}

std::string zoningClassLabel(const LayerDef::FeatureGeom& fg) {
    std::string z = getPropertyValue(fg, "Label");
    if (z.empty()) z = getPropertyValue(fg, "ZONING");
    if (z.empty()) z = getPropertyValue(fg, "ZONE");
    if (z.empty()) z = getPropertyValue(fg, "CLASS");
    if (z.empty()) z = getPropertyValue(fg, "DISTRICT");
    if (z.empty()) return "UNSPECIFIED";
    return z;
}

std::string zoningClassTooltip(const LayerDef::FeatureGeom& fg) {
    std::string z = getPropertyValue(fg, "ZONING");
    if (z.empty()) z = getPropertyValue(fg, "ZONE");
    if (z.empty()) z = getPropertyValue(fg, "CLASS");
    if (z.empty()) z = getPropertyValue(fg, "DISTRICT");
    if (z.empty()) return "UNSPECIFIED";
    return z;
}

ImVec4 colorFromStableKey(const std::string& key) {
    uint32_t h = 2166136261u;
    for (unsigned char c : key) h = (h ^ c) * 16777619u;
    float hue = (float)(h % 360u);
    float sat = 0.62f + (float)((h >> 9) % 25u) / 100.0f;
    float val = 0.74f + (float)((h >> 17) % 20u) / 100.0f;
    sat = std::clamp(sat, 0.55f, 0.85f);
    val = std::clamp(val, 0.65f, 0.95f);
    float c = val * sat;
    float x = c * (1.0f - std::fabs(std::fmod(hue / 60.0f, 2.0f) - 1.0f));
    float m = val - c;
    float r = 0, g = 0, b = 0;
    if (hue < 60) { r = c; g = x; b = 0; }
    else if (hue < 120) { r = x; g = c; b = 0; }
    else if (hue < 180) { r = 0; g = c; b = x; }
    else if (hue < 240) { r = 0; g = x; b = c; }
    else if (hue < 300) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }
    return ImVec4(r + m, g + m, b + m, 1.0f);
}
