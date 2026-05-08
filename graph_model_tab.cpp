#include "graph_model_tab.h"

#include "imgui.h"

void drawGraphModelTab() {
    if (ImGui::BeginTabItem("Graph Model")) {
        ImGui::TextUnformatted("Entity Graph");
        ImGui::Separator();
        ImGui::TextWrapped("Nodes: agency, program, vendor, parcel, tract, award.");
        ImGui::TextWrapped("Edges: funds, regulates, located_in, owns, serves.");
        ImGui::TextDisabled("Hierarchy source: data/government/government_hierarchy_and_pay_2026.json");
        ImGui::TextDisabled("Use for cross-system dependency mapping and path queries.");
        ImGui::EndTabItem();
    }
}
