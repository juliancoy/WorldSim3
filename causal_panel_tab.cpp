#include "causal_panel_tab.h"

#include "imgui.h"

void drawCausalPanelTab() {
    if (ImGui::BeginTabItem("Causal Panel")) {
        ImGui::TextUnformatted("Causal-Ready Panel");
        ImGui::Separator();
        ImGui::TextWrapped("Longitudinal panel by tract with lagged covariates for intervention evaluation.");
        ImGui::TextDisabled("Recommended fields: treatment flag, pre/post windows, controls, lag features.");
        ImGui::EndTabItem();
    }
}
