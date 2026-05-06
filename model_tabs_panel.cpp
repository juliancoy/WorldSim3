#include "model_tabs_panel.h"

#include "imgui.h"

void drawVisualModelTabs(size_t layer_count) {
    const size_t loaded_layer_count = layer_count;

    if (ImGui::BeginTabItem("Graph Model")) {
        ImGui::TextUnformatted("Entity Graph");
        ImGui::Separator();
        ImGui::TextWrapped("Nodes: agency, program, vendor, parcel, tract, award.");
        ImGui::TextWrapped("Edges: funds, regulates, located_in, owns, serves.");
        ImGui::TextDisabled("Hierarchy source: data/government/government_hierarchy_and_pay_2026.json");
        ImGui::TextDisabled("Use for cross-system dependency mapping and path queries.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Star Schema")) {
        ImGui::TextUnformatted("Analytical Star Schema");
        ImGui::Separator();
        ImGui::TextWrapped("Fact tables: incidents, permits, vacancies, tax events, mortgage activity, public health prevalence.");
        ImGui::TextWrapped("Dimensions: time, geography, organization, category, status.");
        ImGui::TextDisabled("Recommended keys: blocklot_id, tract_fips, date_id, agency_id.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Spatial Index")) {
        ImGui::TextUnformatted("Spatial Index Artifacts");
        ImGui::Separator();
        ImGui::TextWrapped("Generate H3/S2 multiresolution tiling views for fast joins and heatmaps.");
        ImGui::Text("Loaded map layers: %zu", loaded_layer_count);
        ImGui::TextDisabled("Recommended resolutions: neighborhood, tract, parcel-adjacent.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Uncertainty")) {
        ImGui::TextUnformatted("Uncertainty-Aware Views");
        ImGui::Separator();
        ImGui::TextWrapped("Track confidence intervals, modeled-estimate flags, and data-quality scores per metric.");
        ImGui::TextDisabled("Critical for CDC PLACES and other modeled prevalence datasets.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Change Log")) {
        ImGui::TextUnformatted("Dataset Change Log");
        ImGui::Separator();
        ImGui::TextWrapped("Snapshot diffs by refresh date: added, removed, changed features and properties.");
        ImGui::TextDisabled("Use for reproducibility and auditability of map-derived decisions.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Risk Scorecards")) {
        ImGui::TextUnformatted("Risk / Composite Scorecards");
        ImGui::Separator();
        ImGui::TextWrapped("Build transparent weighted indicators by tract/CSA with drill-down to source metrics.");
        ImGui::TextDisabled("Publish weights and normalization method per scorecard version.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Causal Panel")) {
        ImGui::TextUnformatted("Causal-Ready Panel");
        ImGui::Separator();
        ImGui::TextWrapped("Longitudinal panel by tract with lagged covariates for intervention evaluation.");
        ImGui::TextDisabled("Recommended fields: treatment flag, pre/post windows, controls, lag features.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Scenarios")) {
        ImGui::TextUnformatted("Scenario Model");
        ImGui::Separator();
        ImGui::TextWrapped("Store baseline plus intervention assumptions and projected outcomes.");
        ImGui::TextDisabled("Use multiple scenario branches for policy alternatives and sensitivity checks.");
        ImGui::EndTabItem();
    }
}
