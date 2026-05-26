#include "render_tile_cache.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace {

uint8_t styleColorByte(float value, uint8_t fallback) {
    if (!std::isfinite(value) || value <= 0.0f) return fallback;
    return static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(value * 255.0f)), 0, 255));
}

}

std::string sanitizeRenderTileCacheComponent(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '_' || ch == '-' || ch == '.') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "unknown" : out;
}

std::string renderPolygonFillStyleKey(const LayerDef& layer, float global_fill_opacity) {
    const uint8_t r = styleColorByte(layer.color.x, 110);
    const uint8_t g = styleColorByte(layer.color.y, 168);
    const uint8_t b = styleColorByte(layer.color.z, 104);
    const float layer_alpha = layer.color.w > 0.0f ? layer.color.w : 0.58f;
    const uint8_t a = static_cast<uint8_t>(std::clamp<int>(
        static_cast<int>(std::lround(std::clamp(layer_alpha * std::clamp(global_fill_opacity, 0.0f, 1.0f), 0.05f, 1.0f) * 255.0f)),
        0,
        255));
    return "fill_r" + std::to_string(r) +
           "_g" + std::to_string(g) +
           "_b" + std::to_string(b) +
           "_a" + std::to_string(a) +
           "_v1";
}

std::filesystem::path renderTileCachePath(
    const std::filesystem::path& root,
    const RenderTileCacheKey& key) {
    const std::string ext = sanitizeRenderTileCacheComponent(key.extension.empty() ? "ppm" : key.extension);
    return root / "data" / "cache" / "render_tiles" /
           sanitizeRenderTileCacheComponent(key.layer_file) /
           sanitizeRenderTileCacheComponent(key.render_route) /
           ("source_" + sanitizeRenderTileCacheComponent(key.source_signature)) /
           ("style_" + sanitizeRenderTileCacheComponent(key.style_key.empty() ? "default" : key.style_key)) /
           ("z" + std::to_string(key.z)) /
           (std::to_string(key.x) + "_" + std::to_string(key.y) + "." + ext);
}
