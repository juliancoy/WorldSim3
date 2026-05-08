#include "star_schema_tab.h"

#include "imgui.h"

void drawStarSchemaTab() {
    if (ImGui::BeginTabItem("Star Schema")) {
        ImGui::TextUnformatted("Analytical Star Schema");
        ImGui::Separator();
        ImGui::TextWrapped("Fact tables: incidents, permits, vacancies, tax events, mortgage activity, public health prevalence.");
        ImGui::TextWrapped("Dimensions: time, geography, organization, category, status.");
        ImGui::TextDisabled("Recommended keys: blocklot_id, tract_fips, date_id, agency_id.");
        ImGui::EndTabItem();
    }
}
