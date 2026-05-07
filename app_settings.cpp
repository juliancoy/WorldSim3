#include "app_settings.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

AppSettings loadAppSettings(const fs::path& root, const AppSettings& defaults) {
    std::ifstream in(root / "data" / "app_settings.json");
    if (!in) return defaults;
    AppSettings out = defaults;
    json j;
    try {
        in >> j;
    } catch (...) {
        return defaults;
    }
    if (j.contains("vulkan_validation_enabled") && j["vulkan_validation_enabled"].is_boolean()) {
        out.vulkan_validation_enabled = j["vulkan_validation_enabled"].get<bool>();
    }
    if (j.contains("grayscale_basemap") && j["grayscale_basemap"].is_boolean()) {
        out.grayscale_basemap = j["grayscale_basemap"].get<bool>();
    }
    if (j.contains("reserve_cpu_cores") && j["reserve_cpu_cores"].is_number_integer()) {
        out.reserve_cpu_cores = std::max(0, j["reserve_cpu_cores"].get<int>());
    }
    return out;
}

void saveAppSettings(const fs::path& root, const AppSettings& settings) {
    fs::create_directories(root / "data");
    json j;
    j["vulkan_validation_enabled"] = settings.vulkan_validation_enabled;
    j["grayscale_basemap"] = settings.grayscale_basemap;
    j["reserve_cpu_cores"] = std::max(0, settings.reserve_cpu_cores);
    std::ofstream out(root / "data" / "app_settings.json");
    if (out) out << j.dump(2);
}
