#include "ui_primitives.h"

#include <algorithm>
#include <cmath>

namespace {
ImVec4 accessibleOnBlack(ImVec4 color) {
    auto channel = [](float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    auto luminance = [&](ImVec4 c) {
        return 0.2126f * channel(c.x) + 0.7152f * channel(c.y) + 0.0722f * channel(c.z);
    };
    color.w = 1.0f;
    const float light = luminance(color);
    const float dark = luminance(ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    return ((std::max(light, dark) + 0.05f) / (std::min(light, dark) + 0.05f)) >= 4.5f
        ? color
        : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}
}

void pushButtonPalette(ButtonPalette palette) {
    switch (palette) {
        case ButtonPalette::Download:
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.12f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.30f, 0.14f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.46f, 0.18f, 0.08f, 1.0f));
            break;
        case ButtonPalette::Destructive:
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.16f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.24f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.44f, 0.12f, 0.10f, 1.0f));
            break;
        case ButtonPalette::ToggleOn:
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.78f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.55f, 0.92f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.34f, 0.62f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            return;
        case ButtonPalette::ToggleOff:
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.16f, 0.16f, 0.22f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.32f, 0.32f, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.42f, 0.42f, 0.42f, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
            return;
    }
}

int buttonPaletteColorCount(ButtonPalette palette) {
    return (palette == ButtonPalette::ToggleOn || palette == ButtonPalette::ToggleOff) ? 4 : 3;
}

void drawLayerNameBadge(const std::string& label, ImVec4 layer_color, size_t max_chars) {
    std::string shown = label.empty() ? std::string("-") : label;
    const std::string full = shown;
    if (max_chars > 3 && shown.size() > max_chars) shown = shown.substr(0, max_chars - 3) + "...";
    const ImVec2 pad(5.0f, 2.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 text_size = ImGui::CalcTextSize(shown.c_str());
    const ImVec2 size(text_size.x + pad.x * 2.0f, text_size.y + pad.y * 2.0f);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0, 0, 0, 255), 2.0f);
    draw_list->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), ImGui::ColorConvertFloat4ToU32(accessibleOnBlack(layer_color)), shown.c_str());
    ImGui::Dummy(size);
    if (shown.size() != full.size() && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 80.0f);
        ImGui::TextUnformatted(full.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool drawIconToggleButton(const char* id, const char* icon, bool& value, const char* tooltip) {
    ImGui::PushID(id);
    const ButtonPalette palette = value ? ButtonPalette::ToggleOn : ButtonPalette::ToggleOff;
    pushButtonPalette(palette);
    bool changed = false;
    if (ImGui::SmallButton(icon)) {
        value = !value;
        changed = true;
    }
    ImGui::PopStyleColor(buttonPaletteColorCount(palette));
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::TextDisabled("%s", value ? "Enabled" : "Disabled");
        ImGui::EndTooltip();
    }
    ImGui::PopID();
    ImGui::SameLine();
    return changed;
}
