#include "vacancy_parcel_tab.h"

#include "imgui.h"

void drawVacancyParcelTab(const VacancyParcelTabContext& ctx) {
    if (!ImGui::BeginTabItem("Vacancy-Parcel")) return;

    size_t parcels_matched = ctx.parcels_matched;
    size_t parcels_geom = ctx.parcels_with_geometry;
    if (ctx.has_selected_owners && ctx.filtered_aggregate_snapshot && ctx.filtered_aggregate_snapshot->valid) {
        parcels_matched = ctx.filtered_aggregate_snapshot->vacancy_parcels_matched;
        parcels_geom = ctx.filtered_aggregate_snapshot->vacancy_parcels_with_geometry;
    }

    ImGui::TextUnformatted("Vacant -> Parcel Join Quality");
    ImGui::Separator();
    ImGui::Text("Vacant notice records: %zu", ctx.notices_total);
    ImGui::Text("Matched to parcel: %zu (%.1f%%)",
                ctx.notices_matched,
                ctx.notices_total ? (100.0 * (double)ctx.notices_matched / (double)ctx.notices_total) : 0.0);
    ImGui::Text("Unmatched notices: %zu", ctx.notices_total >= ctx.notices_matched ? (ctx.notices_total - ctx.notices_matched) : 0);
    ImGui::Separator();
    ImGui::Text("Vacant rehab records: %zu", ctx.rehabs_total);
    ImGui::Text("Matched to parcel: %zu (%.1f%%)",
                ctx.rehabs_matched,
                ctx.rehabs_total ? (100.0 * (double)ctx.rehabs_matched / (double)ctx.rehabs_total) : 0.0);
    ImGui::Text("Unmatched rehabs: %zu", ctx.rehabs_total >= ctx.rehabs_matched ? (ctx.rehabs_total - ctx.rehabs_matched) : 0);
    ImGui::Separator();
    ImGui::Text("Parcels with vacancy evidence: %zu", parcels_matched);
    ImGui::Text("Those with parcel geometry: %zu", parcels_geom);
    if (ctx.has_selected_owners) ImGui::TextDisabled("Counts scoped to selected owners.");
    ImGui::TextWrapped("Map styling uses parcel-level derived status. Raw vacant points are treated as child records.");
    ImGui::EndTabItem();
}
