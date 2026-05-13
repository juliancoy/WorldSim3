#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

struct BasemapCoverage {
    size_t missing = 0;
    size_t total = 0;
};

BasemapCoverage countBasemapCoverage(
    const std::filesystem::path& root,
    const std::string& target_dir,
    int min_zoom,
    int max_native_zoom);
