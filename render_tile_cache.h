#pragma once

#include "types.h"

#include <filesystem>
#include <string>
#include <string_view>

struct RenderTileCacheKey {
    std::string layer_file;
    std::string render_route;
    std::string source_signature;
    std::string style_key;
    int z = 0;
    int x = 0;
    int y = 0;
    std::string extension = "ppm";
};

std::string sanitizeRenderTileCacheComponent(std::string_view value);
std::string renderPolygonFillStyleKey(const LayerDef& layer, float global_fill_opacity);
std::filesystem::path renderTileCachePath(
    const std::filesystem::path& root,
    const RenderTileCacheKey& key);
