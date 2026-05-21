#include "feature_props.h"

#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace {
std::mutex g_feature_property_registry_mutex;
std::unordered_map<const LayerDef::FeatureRecord*, const LayerDef::FeatureProperties*> g_feature_property_registry;
std::unordered_map<const LayerDef::FeatureRecord*, FeaturePropertyPairs> g_transient_feature_property_registry;
}

void rebuildFeaturePropertyRegistryForLayer(const LayerDef& layer) {
    std::lock_guard<std::mutex> lk(g_feature_property_registry_mutex);
    for (const auto& fg : layer.features) {
        g_feature_property_registry.erase(&fg);
    }
    const size_t count = std::min(layer.features.size(), layer.feature_properties.size());
    for (size_t i = 0; i < count; ++i) {
        g_feature_property_registry[&layer.features[i]] = &layer.feature_properties[i];
    }
}

void clearFeaturePropertyRegistryForLayer(const LayerDef& layer) {
    std::lock_guard<std::mutex> lk(g_feature_property_registry_mutex);
    for (const auto& fg : layer.features) {
        g_feature_property_registry.erase(&fg);
        g_transient_feature_property_registry.erase(&fg);
    }
}

const LayerDef::FeatureProperties* getFeatureProperties(const LayerDef& layer, size_t feature_idx) {
    if (feature_idx >= layer.feature_properties.size()) return nullptr;
    return &layer.feature_properties[feature_idx];
}

const FeaturePropertyPairs* getPropertyPairs(const LayerDef::FeatureRecord& fg) {
    std::lock_guard<std::mutex> lk(g_feature_property_registry_mutex);
    auto tit = g_transient_feature_property_registry.find(&fg);
    if (tit != g_transient_feature_property_registry.end()) return &tit->second;
    auto it = g_feature_property_registry.find(&fg);
    if (it != g_feature_property_registry.end() && it->second) return &it->second->values;
    return nullptr;
}

const FeaturePropertyPairs* getTransientFeatureProperties(const LayerDef::FeatureRecord& fg) {
    std::lock_guard<std::mutex> lk(g_feature_property_registry_mutex);
    auto it = g_transient_feature_property_registry.find(&fg);
    return it == g_transient_feature_property_registry.end() ? nullptr : &it->second;
}

void setTransientFeatureProperties(LayerDef::FeatureRecord& fg, FeaturePropertyPairs values) {
    std::lock_guard<std::mutex> lk(g_feature_property_registry_mutex);
    g_transient_feature_property_registry[&fg] = std::move(values);
}

void clearTransientFeatureProperties(const LayerDef::FeatureRecord& fg) {
    std::lock_guard<std::mutex> lk(g_feature_property_registry_mutex);
    g_transient_feature_property_registry.erase(&fg);
}

std::string getPropertyValue(const LayerDef::FeatureRecord& fg, const std::string& key) {
    std::lock_guard<std::mutex> lk(g_feature_property_registry_mutex);
    auto tit = g_transient_feature_property_registry.find(&fg);
    if (tit != g_transient_feature_property_registry.end()) {
        for (const auto& kv : tit->second) {
            if (kv.first == key) return kv.second;
        }
    }
    auto it = g_feature_property_registry.find(&fg);
    if (it != g_feature_property_registry.end() && it->second) {
        for (const auto& kv : it->second->values) {
            if (kv.first == key) return kv.second;
        }
    }
    return "";
}

std::string getPropertyValue(const LayerDef& layer, size_t feature_idx, const std::string& key) {
    if (const LayerDef::FeatureProperties* props = getFeatureProperties(layer, feature_idx)) {
        for (const auto& kv : props->values) {
            if (kv.first == key) return kv.second;
        }
    }
    if (feature_idx >= layer.features.size()) return "";
    return getPropertyValue(layer.features[feature_idx], key);
}

std::string normalizeJoinKey(std::string s) {
    size_t write_idx = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char u = (unsigned char)s[i];
        const bool is_digit = (u >= (unsigned char)'0' && u <= (unsigned char)'9');
        const bool is_upper = (u >= (unsigned char)'A' && u <= (unsigned char)'Z');
        const bool is_lower = (u >= (unsigned char)'a' && u <= (unsigned char)'z');
        if (!(is_digit || is_upper || is_lower)) continue;
        s[write_idx++] = is_lower ? (char)(u - ((unsigned char)'a' - (unsigned char)'A')) : (char)u;
    }
    s.resize(write_idx);
    return s;
}

std::string zoningClassKey(const LayerDef::FeatureRecord& fg) {
    std::string z = getPropertyValue(fg, "Zoning");
    if (z.empty()) z = getPropertyValue(fg, "Label");
    if (z.empty()) z = getPropertyValue(fg, "ZoningLabel");
    if (z.empty()) z = getPropertyValue(fg, "ZONING");
    if (z.empty()) z = getPropertyValue(fg, "ZONED");
    if (z.empty()) z = getPropertyValue(fg, "ZONE");
    if (z.empty()) z = getPropertyValue(fg, "ZONE_CLASS");
    if (z.empty()) z = getPropertyValue(fg, "ZONE_DIST");
    if (z.empty()) z = getPropertyValue(fg, "CLASS");
    if (z.empty()) z = getPropertyValue(fg, "DISTRICT");
    if (z.empty()) z = getPropertyValue(fg, "Type");
    if (z.empty()) z = getPropertyValue(fg, "TYPE");
    if (z.empty()) z = getPropertyValue(fg, "DIST_CODE");
    if (z.empty()) return "UNSPECIFIED";
    return z;
}

std::string zoningClassKey(const LayerDef& layer, size_t feature_idx) {
    std::string z = getPropertyValue(layer, feature_idx, "Zoning");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "Label");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZoningLabel");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZONING");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZONED");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZONE");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZONE_CLASS");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZONE_DIST");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "CLASS");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "DISTRICT");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "Type");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "TYPE");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "DIST_CODE");
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

std::string zoningClassLabel(const LayerDef::FeatureRecord& fg) {
    std::string z = getPropertyValue(fg, "Label");
    if (z.empty()) z = getPropertyValue(fg, "ZONING");
    if (z.empty()) z = getPropertyValue(fg, "ZONED");
    if (z.empty()) z = getPropertyValue(fg, "ZONE");
    if (z.empty()) z = getPropertyValue(fg, "ZONE_CLASS");
    if (z.empty()) z = getPropertyValue(fg, "ZONE_DIST");
    if (z.empty()) z = getPropertyValue(fg, "CLASS");
    if (z.empty()) z = getPropertyValue(fg, "DISTRICT");
    if (z.empty()) z = getPropertyValue(fg, "Type");
    if (z.empty()) z = getPropertyValue(fg, "TYPE");
    if (z.empty()) z = getPropertyValue(fg, "DIST_CODE");
    if (z.empty()) return "UNSPECIFIED";
    return z;
}

std::string zoningClassLabel(const LayerDef& layer, size_t feature_idx) {
    std::string z = getPropertyValue(layer, feature_idx, "Label");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZONING");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZONED");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZONE");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZONE_CLASS");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZONE_DIST");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "CLASS");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "DISTRICT");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "Type");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "TYPE");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "DIST_CODE");
    if (z.empty()) return "UNSPECIFIED";
    return z;
}

std::string zoningClassTooltip(const LayerDef::FeatureRecord& fg) {
    std::string z = getPropertyValue(fg, "ZONING");
    if (z.empty()) z = getPropertyValue(fg, "ZONED");
    if (z.empty()) z = getPropertyValue(fg, "ZONE");
    if (z.empty()) z = getPropertyValue(fg, "ZONE_CLASS");
    if (z.empty()) z = getPropertyValue(fg, "ZONE_DIST");
    if (z.empty()) z = getPropertyValue(fg, "CLASS");
    if (z.empty()) z = getPropertyValue(fg, "DISTRICT");
    if (z.empty()) z = getPropertyValue(fg, "Type");
    if (z.empty()) z = getPropertyValue(fg, "TYPE");
    if (z.empty()) z = getPropertyValue(fg, "DIST_CODE");
    if (z.empty()) return "UNSPECIFIED";
    return z;
}

std::string zoningClassTooltip(const LayerDef& layer, size_t feature_idx) {
    std::string z = getPropertyValue(layer, feature_idx, "ZONING");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZONED");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZONE");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZONE_CLASS");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "ZONE_DIST");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "CLASS");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "DISTRICT");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "Type");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "TYPE");
    if (z.empty()) z = getPropertyValue(layer, feature_idx, "DIST_CODE");
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

static ImVec4 hsvToRgb(float hue_deg, float sat, float val) {
    float hue = std::fmod(hue_deg, 360.0f);
    if (hue < 0.0f) hue += 360.0f;
    const float c = val * sat;
    const float x = c * (1.0f - std::fabs(std::fmod(hue / 60.0f, 2.0f) - 1.0f));
    const float m = val - c;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (hue < 60.0f) { r = c; g = x; b = 0.0f; }
    else if (hue < 120.0f) { r = x; g = c; b = 0.0f; }
    else if (hue < 180.0f) { r = 0.0f; g = c; b = x; }
    else if (hue < 240.0f) { r = 0.0f; g = x; b = c; }
    else if (hue < 300.0f) { r = x; g = 0.0f; b = c; }
    else { r = c; g = 0.0f; b = x; }
    return ImVec4(r + m, g + m, b + m, 1.0f);
}

static void rgbToHsv(const ImVec4& rgb, float& hue_deg, float& sat, float& val) {
    const float r = rgb.x;
    const float g = rgb.y;
    const float b = rgb.z;
    const float cmax = std::max({r, g, b});
    const float cmin = std::min({r, g, b});
    const float delta = cmax - cmin;

    val = cmax;
    sat = (cmax <= 0.0f) ? 0.0f : (delta / cmax);
    hue_deg = 0.0f;
    if (delta <= 1e-6f) return;

    if (cmax == r) hue_deg = 60.0f * std::fmod(((g - b) / delta), 6.0f);
    else if (cmax == g) hue_deg = 60.0f * (((b - r) / delta) + 2.0f);
    else hue_deg = 60.0f * (((r - g) / delta) + 4.0f);
    if (hue_deg < 0.0f) hue_deg += 360.0f;
}

enum class ZoningFamily {
    Residential,
    Commercial,
    Industrial,
    MixedUse,
    OfficeEmployment,
    CivicInstitutional,
    OpenSpaceConservation,
    AgricultureRural,
    DowntownCenter,
    OverlaySpecial,
    Other
};

static bool hasAnyToken(const std::string& value, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (value.find(needle) != std::string::npos) return true;
    }
    return false;
}

static ZoningFamily classifyZoningFamily(const std::string& zone_key_upper) {
    const std::string& u = zone_key_upper;
    if (u.empty() || u == "UNSPECIFIED") return ZoningFamily::Other;

    if (hasAnyToken(u, {"OV", "OVERLAY", "SP", "SPECIAL", "FLOOD", "HIST", "AIR", "CHES", "BUFFER"})) {
        return ZoningFamily::OverlaySpecial;
    }
    if (hasAnyToken(u, {"OS", "OPEN", "PARK", "REC", "GREEN", "CONSERV", "PRESERV", "RESOURCE", "WETLAND"})) {
        return ZoningFamily::OpenSpaceConservation;
    }
    if (hasAnyToken(u, {"AG", "AGR", "AGRIC", "RURAL", "RR", "AR", "RC", "RA"})) {
        return ZoningFamily::AgricultureRural;
    }
    if (hasAnyToken(u, {"MX", "MU", "MIXED", "TOD", "TRANSIT", "TC", "TOWNCENTER", "VILLAGE", "VC", "CORRIDOR"})) {
        return ZoningFamily::MixedUse;
    }
    if (hasAnyToken(u, {"DT", "CBD", "DOWNTOWN", "CENTER", "MAINST", "URBANCORE"})) {
        return ZoningFamily::DowntownCenter;
    }
    if (hasAnyToken(u, {"IND", "INDUSTRIAL", "WARE", "LOG", "FLEX", "HI", "HEAVY", "LI", "LIGHT", "IH", "IL", "M-"})) {
        return ZoningFamily::Industrial;
    }
    if (hasAnyToken(u, {"OFFICE", "OFF", "EMP", "EMPLOY", "BUSPARK", "BP", "RESEARCH", "CORP", "EO", "EC"})) {
        return ZoningFamily::OfficeEmployment;
    }
    if (hasAnyToken(u, {"INST", "INSTIT", "CIV", "PUBLIC", "SCHOOL", "CAMPUS", "GOV", "HOSP", "MED"})) {
        return ZoningFamily::CivicInstitutional;
    }
    if (hasAnyToken(u, {"COM", "COMMERCIAL", "BUS", "RETAIL", "SHOP", "CC", "CG", "CN", "CR", "B-", "B1", "B2", "B3", "BL", "BM", "BR"})) {
        return ZoningFamily::Commercial;
    }
    if (hasAnyToken(u, {"RES", "RESIDENTIAL", "APT", "APART", "MULTI", "SINGLE", "TOWNHOUSE", "ROW", "RM", "RH", "RE", "RO", "RS", "DR", "R-"})) {
        return ZoningFamily::Residential;
    }
    if (!u.empty()) {
        if (u[0] == 'R') return ZoningFamily::Residential;
        if (u[0] == 'C' || u[0] == 'B') return ZoningFamily::Commercial;
        if (u[0] == 'I' || u[0] == 'M') return ZoningFamily::Industrial;
        if (u[0] == 'O' || u[0] == 'E') return ZoningFamily::OfficeEmployment;
    }
    return ZoningFamily::Other;
}

ImVec4 zoningColorFromConvention(const std::string& zone_key) {
    std::string u;
    u.reserve(zone_key.size());
    for (char ch : zone_key) u.push_back((char)std::toupper((unsigned char)ch));

    uint32_t h = 2166136261u;
    for (unsigned char c : u) h = (h ^ c) * 16777619u;
    const float sat_jitter = (float)((h >> 9) % 12u) / 100.0f;
    const float val_jitter = (float)((h >> 17) % 14u) / 100.0f;
    switch (classifyZoningFamily(u)) {
        case ZoningFamily::Residential:
            return hsvToRgb(128.0f + (float)(h % 10u), 0.62f + sat_jitter, 0.66f + val_jitter);
        case ZoningFamily::Commercial:
            return hsvToRgb(210.0f + (float)(h % 12u), 0.64f + sat_jitter, 0.70f + val_jitter);
        case ZoningFamily::Industrial:
            return hsvToRgb(53.0f + (float)(h % 8u), 0.70f + sat_jitter, 0.78f + val_jitter);
        case ZoningFamily::MixedUse:
            return hsvToRgb(282.0f + (float)(h % 10u), 0.54f + sat_jitter, 0.73f + val_jitter);
        case ZoningFamily::OfficeEmployment:
            return hsvToRgb(190.0f + (float)(h % 10u), 0.56f + sat_jitter, 0.72f + val_jitter);
        case ZoningFamily::CivicInstitutional:
            return hsvToRgb(24.0f + (float)(h % 10u), 0.60f + sat_jitter, 0.78f + val_jitter);
        case ZoningFamily::OpenSpaceConservation:
            return hsvToRgb(150.0f + (float)(h % 8u), 0.42f + sat_jitter, 0.66f + val_jitter);
        case ZoningFamily::AgricultureRural:
            return hsvToRgb(82.0f + (float)(h % 10u), 0.52f + sat_jitter, 0.68f + val_jitter);
        case ZoningFamily::DowntownCenter:
            return hsvToRgb(336.0f + (float)(h % 10u), 0.58f + sat_jitter, 0.76f + val_jitter);
        case ZoningFamily::OverlaySpecial:
            return hsvToRgb(6.0f + (float)(h % 10u), 0.40f + sat_jitter, 0.78f + val_jitter);
        case ZoningFamily::Other:
            break;
    }
    return hsvToRgb(0.0f, 0.0f, 0.58f + val_jitter * 0.5f);
}

ImVec4 zoningShadeVariant(const ImVec4& base_color, const std::string& zone_key) {
    uint32_t h = 2166136261u;
    for (unsigned char c : zone_key) h = (h ^ c) * 16777619u;

    float base_h = 0.0f;
    float base_s = 0.0f;
    float base_v = 0.0f;
    rgbToHsv(base_color, base_h, base_s, base_v);

    const float hue_shift = ((float)((h >> 5) % 31u) - 15.0f) * 0.8f;     // about -12..+12 deg
    const float sat_shift = ((float)((h >> 13) % 29u) - 14.0f) / 110.0f;  // about -0.13..+0.13
    const float val_shift = ((float)((h >> 21) % 37u) - 18.0f) / 95.0f;   // about -0.19..+0.19

    const float next_h = std::fmod(base_h + hue_shift + 360.0f, 360.0f);
    const float next_s = std::clamp(base_s + sat_shift, 0.35f, 0.95f);
    const float next_v = std::clamp(base_v + val_shift, 0.42f, 0.97f);
    return hsvToRgb(next_h, next_s, next_v);
}
