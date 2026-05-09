#pragma once

#include "imgui.h"

void drawMapStatusBadge(ImDrawList* draw, ImVec2 origin, const char* label);
void drawMapZoomBadge(ImDrawList* draw, ImVec2 origin, ImVec2 size, int zoom);
