#include "scenarios_tab.h"

#include "imgui.h"

void drawScenariosTab() {
    if (ImGui::BeginTabItem("Scenarios")) {
        ImGui::TextUnformatted("Scenario Model");
        ImGui::Separator();
        ImGui::TextWrapped("Store baseline plus intervention assumptions and projected outcomes.");
        ImGui::TextDisabled("Use multiple scenario branches for policy alternatives and sensitivity checks.");
        ImGui::EndTabItem();
    }
}
