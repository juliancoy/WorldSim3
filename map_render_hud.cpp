#include "map_render_hud.h"

#include <cstdio>

void drawMapStatusBadge(ImDrawList* draw, ImVec2 origin, const char* label) {
    if (!draw || !label || !*label) return;
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const float pad_x = 12.0f;
    const float pad_y = 7.0f;
    const ImVec2 box_min(origin.x + 12.0f, origin.y + 12.0f);
    const ImVec2 box_max(box_min.x + text_size.x + pad_x * 2.0f, box_min.y + text_size.y + pad_y * 2.0f);
    draw->AddRectFilled(box_min, box_max, IM_COL32(17, 24, 32, 220), 8.0f);
    draw->AddRect(box_min, box_max, IM_COL32(255, 255, 255, 80), 8.0f);
    draw->AddText(ImVec2(box_min.x + pad_x, box_min.y + pad_y), IM_COL32(245, 248, 250, 245), label);
}

void drawMapZoomBadge(ImDrawList* draw, ImVec2 origin, ImVec2 size, int zoom) {
    if (!draw) return;
    char zoom_label[32];
    std::snprintf(zoom_label, sizeof(zoom_label), "Zoom %d", zoom);
    const ImVec2 text_size = ImGui::CalcTextSize(zoom_label);
    const float pad_x = 10.0f;
    const float pad_y = 6.0f;
    const ImVec2 box_max(origin.x + size.x - 12.0f, origin.y + size.y - 12.0f);
    const ImVec2 box_min(box_max.x - text_size.x - pad_x * 2.0f, box_max.y - text_size.y - pad_y * 2.0f);
    draw->AddRectFilled(box_min, box_max, IM_COL32(17, 24, 32, 205), 7.0f);
    draw->AddRect(box_min, box_max, IM_COL32(255, 255, 255, 70), 7.0f);
    draw->AddText(ImVec2(box_min.x + pad_x, box_min.y + pad_y), IM_COL32(245, 248, 250, 245), zoom_label);
}
