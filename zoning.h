#pragma once

#include "imgui.h"

#include <filesystem>
#include <string>
#include <unordered_map>

struct ZoneMetadata {
    std::string label;
    std::string description;
    std::string color_hex;
    ImVec4 color = ImVec4(0, 0, 0, 1);
    bool has_color = false;
};

bool parseHexColor(const std::string& hex, ImVec4& out);
std::unordered_map<std::string, ZoneMetadata> loadZoneMetadata(const std::filesystem::path& root);
