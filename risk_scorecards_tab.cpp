#include "risk_scorecards_tab.h"

#include "imgui.h"

void drawRiskScorecardsTab() {
    if (ImGui::BeginTabItem("Risk Scorecards")) {
        ImGui::TextUnformatted("Risk / Composite Scorecards");
        ImGui::Separator();
        ImGui::TextWrapped("Build transparent weighted indicators by tract/CSA with drill-down to source metrics.");
        ImGui::TextDisabled("Publish weights and normalization method per scorecard version.");
        ImGui::EndTabItem();
    }
}
