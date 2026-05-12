#include "map_render_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

bool tryGetFeaturePropertyFloat(const LayerDef::FeatureGeom& fg, const std::string& key, float& out) {
    if (key.empty()) return false;
    for (const auto& kv : fg.properties) {
        if (kv.first != key) continue;
        char* end = nullptr;
        const float v = std::strtof(kv.second.c_str(), &end);
        if (end == kv.second.c_str() || (end && *end != '\0')) return false;
        if (!std::isfinite(v)) return false;
        out = v;
        return true;
    }
    return false;
}

ImVec4 heatColor(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const ImVec4 cold(0.12f, 0.35f, 0.75f, 1.0f);
    const ImVec4 mid(0.98f, 0.83f, 0.26f, 1.0f);
    const ImVec4 hot(0.82f, 0.14f, 0.12f, 1.0f);
    auto lerp = [](const ImVec4& a, const ImVec4& b, float u) {
        return ImVec4(
            a.x + (b.x - a.x) * u,
            a.y + (b.y - a.y) * u,
            a.z + (b.z - a.z) * u,
            a.w + (b.w - a.w) * u);
    };
    if (t < 0.5f) return lerp(cold, mid, t * 2.0f);
    return lerp(mid, hot, (t - 0.5f) * 2.0f);
}

float applyPowerGamma(float t, float gamma) {
    t = std::clamp(t, 0.0f, 1.0f);
    gamma = std::clamp(gamma, 0.10f, 5.0f);
    return std::pow(t, gamma);
}

ImU32 colorWithAlpha(const ImVec4& c, int alpha) {
    const int r = std::clamp((int)std::lround(c.x * 255.0f), 0, 255);
    const int g = std::clamp((int)std::lround(c.y * 255.0f), 0, 255);
    const int b = std::clamp((int)std::lround(c.z * 255.0f), 0, 255);
    return IM_COL32(r, g, b, alpha);
}

ImVec4 darkenColor(const ImVec4& c, float mul) {
    return ImVec4(
        std::clamp(c.x * mul, 0.0f, 1.0f),
        std::clamp(c.y * mul, 0.0f, 1.0f),
        std::clamp(c.z * mul, 0.0f, 1.0f),
        1.0f);
}

ImVec4 blendVacancyColor(const ImVec4& notice_color, const ImVec4& rehab_color, int notice_count, int rehab_count) {
    if (notice_count > 0 && rehab_count > 0) {
        return ImVec4(
            (notice_color.x + rehab_color.x) * 0.5f,
            (notice_color.y + rehab_color.y) * 0.5f,
            (notice_color.z + rehab_color.z) * 0.5f,
            1.0f);
    }
    if (rehab_count > 0) return rehab_color;
    return notice_color;
}

ImVec4 blendTaxColor(
    const ImVec4& lien_color,
    const ImVec4& sale_color,
    bool lien_enabled,
    bool sale_enabled,
    int lien_count,
    int sale_count) {
    if (lien_enabled && sale_enabled && lien_count > 0 && sale_count > 0) {
        return ImVec4(
            (lien_color.x + sale_color.x) * 0.5f,
            (lien_color.y + sale_color.y) * 0.5f,
            (lien_color.z + sale_color.z) * 0.5f,
            1.0f);
    }
    if (sale_enabled && sale_count > 0) return sale_color;
    return lien_color;
}

int overlayWeight(bool first_enabled, int first_count, bool second_enabled, int second_count) {
    int weight = 0;
    if (first_enabled) weight += first_count;
    if (second_enabled) weight += second_count;
    return weight;
}

int scaledOverlayAlpha(int base, int per_weight, int min_alpha, int max_alpha, int weight) {
    return std::clamp(base + weight * per_weight, min_alpha, max_alpha);
}

void hashCombineU64(uint64_t& seed, uint64_t v) {
    seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

void hashCombineFloat(uint64_t& seed, float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    hashCombineU64(seed, (uint64_t)bits);
}

void hashCombineQuantizedDouble(uint64_t& seed, double v, double scale) {
    if (!std::isfinite(v)) {
        hashCombineU64(seed, 0xffffffffffffffffULL);
        return;
    }
    const int64_t q = (int64_t)std::llround(v * scale);
    hashCombineU64(seed, (uint64_t)q);
}
