#include "map_overlay_panels.h"

#include "model_tabs_panel.h"

#include <algorithm>

void drawMapOverlayPanelsPopup(
    ImVec2 origin,
    ImVec2 size,
    TimeCubePanelContext& time_cube_panel_ctx,
    PolicyPanelContext& policy_panel_ctx,
    size_t layer_count) {
    ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x - 34.0f, origin.y + 8.0f));
    if (ImGui::SmallButton("...")) ImGui::OpenPopup("map_overlay_panels_popup");
    ImGui::SetNextWindowSize(
        ImVec2(std::min(980.0f, size.x - 24.0f), std::min(720.0f, size.y - 24.0f)),
        ImGuiCond_Appearing);
    if (ImGui::BeginPopup("map_overlay_panels_popup")) {
        if (ImGui::BeginTabBar("map_overlay_panels_tabs")) {
            drawTimeCubeTab(time_cube_panel_ctx);
            drawPolicyHierarchyTab(policy_panel_ctx);
            drawVisualModelTabs(layer_count);
            ImGui::EndTabBar();
        }
        ImGui::EndPopup();
    }
}
