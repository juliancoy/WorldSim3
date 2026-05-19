#include "cpu_zoning_fallback.h"

#include "app_utils.h"

#include <cstdlib>

namespace {
bool isZoningPolygonLayerCpuFallback(const LayerDef& layer) {
    if (layerUsesPointGeometry(layer)) return false;
    if (layer.category == LayerDef::Category::Zoning) return true;
    return containsCaseInsensitive(layer.file, "zoning") ||
           containsCaseInsensitive(layer.name, "zoning");
}
}

bool cpuZoningFallbackExplicitlyApproved() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    const char* env = std::getenv("WS3_ALLOW_CPU_ZONING_FALLBACK");
    cached = (env && env[0] == '1' && env[1] == '\0') ? 1 : 0;
    return cached == 1;
}

bool cpuZoningFillAllowed(const LayerDef& layer) {
    return isZoningPolygonLayerCpuFallback(layer) && cpuZoningFallbackExplicitlyApproved();
}

bool cpuZoningOutlineAllowed(const LayerDef& layer) {
    return isZoningPolygonLayerCpuFallback(layer) && cpuZoningFallbackExplicitlyApproved();
}
