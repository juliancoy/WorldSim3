#include "change_log_tab.h"

#include "imgui.h"

void drawChangeLogTab() {
    if (ImGui::BeginTabItem("Change Log")) {
        ImGui::TextUnformatted("Dataset Change Log");
        ImGui::Separator();
        ImGui::TextWrapped("Snapshot diffs by refresh date: added, removed, changed features and properties.");
        ImGui::TextDisabled("Use for reproducibility and auditability of map-derived decisions.");
        ImGui::EndTabItem();
    }
}
