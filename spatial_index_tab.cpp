#include "spatial_index_tab.h"

#include "imgui.h"

void drawSpatialIndexTab(size_t layer_count) {
    if (ImGui::BeginTabItem("Spatial Index")) {
        ImGui::TextUnformatted("Spatial Index Artifacts");
        ImGui::Separator();
        ImGui::TextWrapped("Generate H3/S2 multiresolution tiling views for fast joins and heatmaps.");
        ImGui::Text("Loaded map layers: %zu", layer_count);
        ImGui::TextDisabled("Recommended resolutions: neighborhood, tract, parcel-adjacent.");
        ImGui::EndTabItem();
    }
}
