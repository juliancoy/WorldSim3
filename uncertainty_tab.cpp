#include "uncertainty_tab.h"

#include "imgui.h"

void drawUncertaintyTab() {
    if (ImGui::BeginTabItem("Uncertainty")) {
        ImGui::TextUnformatted("Uncertainty-Aware Views");
        ImGui::Separator();
        ImGui::TextWrapped("Track confidence intervals, modeled-estimate flags, and data-quality scores per metric.");
        ImGui::TextDisabled("Critical for CDC PLACES and other modeled prevalence datasets.");
        ImGui::EndTabItem();
    }
}
