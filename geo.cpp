#include "geo.h"

#include <cmath>

ImVec2 lonLatToWorldPx(double lon, double lat, int zoom) {
    const double scale = 256.0 * std::pow(2.0, zoom);
    const double x = (lon + 180.0) / 360.0 * scale;
    const double lat_rad = lat * M_PI / 180.0;
    const double y = (1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / M_PI) / 2.0 * scale;
    return ImVec2((float)x, (float)y);
}

ImVec2 worldPxToLonLat(const ImVec2& world, int zoom) {
    const double scale = 256.0 * std::pow(2.0, zoom);
    const double lon = (world.x / scale) * 360.0 - 180.0;
    const double n = M_PI - 2.0 * M_PI * world.y / scale;
    const double lat = 180.0 / M_PI * std::atan(std::sinh(n));
    return ImVec2((float)lon, (float)lat);
}

ImVec2 worldToScreen(const ImVec2& world, const ImVec2& center_world, const ImVec2& origin, const ImVec2& size, double scale) {
    return ImVec2(
        origin.x + size.x * 0.5f + (float)((world.x - center_world.x) * scale),
        origin.y + size.y * 0.5f + (float)((world.y - center_world.y) * scale));
}
