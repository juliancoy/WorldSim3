#pragma once

#include <filesystem>

struct AppSettings {
    bool vulkan_validation_enabled = false;
    bool grayscale_basemap = false;
};

AppSettings loadAppSettings(const std::filesystem::path& root, const AppSettings& defaults);
void saveAppSettings(const std::filesystem::path& root, const AppSettings& settings);
