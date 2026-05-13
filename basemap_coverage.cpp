#include "basemap_coverage.h"

#include "app_utils.h"

#include <algorithm>
#include <system_error>

BasemapCoverage countBasemapCoverage(
    const std::filesystem::path& root,
    const std::string& target_dir,
    int min_zoom,
    int max_native_zoom) {
    const std::filesystem::path base = root / "data" / target_dir;
    const double min_lon = -76.75;
    const double max_lon = -76.45;
    const double min_lat = 39.18;
    const double max_lat = 39.40;
    BasemapCoverage coverage;
    for (int z = min_zoom; z <= max_native_zoom; ++z) {
        auto [x0, y_max] = deg2num(min_lat, min_lon, z);
        auto [x1, y_min] = deg2num(max_lat, max_lon, z);
        if (x0 > x1) std::swap(x0, x1);
        if (y_min > y_max) std::swap(y_min, y_max);
        for (int x = x0; x <= x1; ++x) {
            for (int y = y_min; y <= y_max; ++y) {
                coverage.total++;
                std::error_code ec;
                const std::filesystem::path out =
                    base / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png");
                if (!(std::filesystem::exists(out, ec) && !ec)) coverage.missing++;
            }
        }
    }
    return coverage;
}
