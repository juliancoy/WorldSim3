#pragma once

#include "imgui.h"

#include <string>

enum class ButtonPalette {
    Download,
    Destructive,
    ToggleOn,
    ToggleOff
};

void pushButtonPalette(ButtonPalette palette);
int buttonPaletteColorCount(ButtonPalette palette);
void drawLayerNameBadge(const std::string& label, ImVec4 layer_color, size_t max_chars = 0);
bool drawIconToggleButton(const char* id, const char* icon, bool& value, const char* tooltip);
