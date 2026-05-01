#pragma once

#include "imgui.h"

ImVec2 lonLatToWorldPx(double lon, double lat, int zoom);
ImVec2 worldPxToLonLat(const ImVec2& world, int zoom);
ImVec2 worldToScreen(const ImVec2& world, const ImVec2& center_world, const ImVec2& origin, const ImVec2& size, double scale);
