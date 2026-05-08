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

ImVec4 zoningColorFromConvention(const std::string& zone_key) {
    std::string u;
    u.reserve(zone_key.size());
    for (char ch : zone_key) u.push_back((char)std::toupper((unsigned char)ch));

    uint32_t h = 2166136261u;
    for (unsigned char c : u) h = (h ^ c) * 16777619u;
    const float sat_jitter = (float)((h >> 9) % 12u) / 100.0f;
    const float val_jitter = (float)((h >> 17) % 14u) / 100.0f;

    // SimCity convention: residential=green, commercial=blue, industrial=yellow.
    bool residential = false;
    bool commercial = false;
    bool industrial = false;
    if (!u.empty()) {
        residential = (u[0] == 'R') || (u.find("RES") != std::string::npos);
        commercial = (u[0] == 'C') || (u.find("COM") != std::string::npos) || (u.find("BUS") != std::string::npos);
        industrial = (u[0] == 'I') || (u.find("IND") != std::string::npos) || (u[0] == 'M');
    }

    if (residential) return hsvToRgb(128.0f + (float)(h % 10u), 0.62f + sat_jitter, 0.66f + val_jitter);
    if (commercial) return hsvToRgb(210.0f + (float)(h % 12u), 0.64f + sat_jitter, 0.70f + val_jitter);
    if (industrial) return hsvToRgb(53.0f + (float)(h % 8u), 0.70f + sat_jitter, 0.78f + val_jitter);

    // Remaining zoning buckets get distinct conventional planning hues.
    if (u.find("MX") != std::string::npos || u.find("MU") != std::string::npos) {
        return hsvToRgb(282.0f + (float)(h % 10u), 0.54f + sat_jitter, 0.73f + val_jitter); // mixed-use
    }
    if (u.find("OS") != std::string::npos || u.find("OPEN") != std::string::npos || u.find("PARK") != std::string::npos) {
        return hsvToRgb(150.0f + (float)(h % 8u), 0.42f + sat_jitter, 0.66f + val_jitter); // open space
    }
    if (u.find("DT") != std::string::npos || u.find("DOWNTOWN") != std::string::npos) {
        return hsvToRgb(336.0f + (float)(h % 10u), 0.58f + sat_jitter, 0.76f + val_jitter); // downtown/overlay
    }
    if (u.find("CIV") != std::string::npos || u.find("INS") != std::string::npos || u.find("INST") != std::string::npos) {
        return hsvToRgb(24.0f + (float)(h % 10u), 0.60f + sat_jitter, 0.78f + val_jitter); // civic/institutional
    }
    return hsvToRgb(0.0f, 0.0f, 0.58f + val_jitter * 0.5f); // other/unspecified neutral gray
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
