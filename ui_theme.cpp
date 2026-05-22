#include "ui_theme.h"

#include "imgui.h"

void applyWorldsimUiTheme(bool dark_mode) {
    if (!dark_mode) {
        ImGui::StyleColorsLight();
        return;
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.064f, 0.075f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.070f, 0.080f, 0.095f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.060f, 0.070f, 0.085f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.260f, 0.300f, 0.350f, 0.70f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.110f, 0.130f, 0.155f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.160f, 0.200f, 0.240f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.200f, 0.260f, 0.310f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.070f, 0.085f, 0.105f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.100f, 0.130f, 0.160f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.080f, 0.095f, 0.115f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.120f, 0.210f, 0.260f, 0.85f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.150f, 0.300f, 0.360f, 0.90f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.180f, 0.380f, 0.450f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.120f, 0.240f, 0.300f, 0.95f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.160f, 0.350f, 0.420f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.100f, 0.480f, 0.560f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.090f, 0.130f, 0.160f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.160f, 0.340f, 0.420f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.120f, 0.260f, 0.320f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.300f, 0.800f, 0.900f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.260f, 0.620f, 0.700f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.340f, 0.820f, 0.920f, 1.00f);

    style.WindowRounding = 5.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 5.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 4.0f;
}
