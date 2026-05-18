#include "active_queries_tab.h"
#include "right_panel.h"

#include "filter_context_builder.h"
#include "filters.h"
#include "filters_tab.h"
#include "gradient_tab.h"
#include "imgui.h"
#include "owner_info.h"
#include "owners_tab.h"
#include "selection.h"
#include "sql_tab.h"
#include "vacancy_parcel_tab.h"

void drawRightPanelWindow(const RightPanelContext& ctx) {
    if (!ctx.root || !ctx.duckdb_analytics || !ctx.layers || !ctx.unified_parcels || !ctx.map_filter_state ||
        !ctx.query_layers || !ctx.zoning_metadata || !ctx.zoning_zone_enabled || !ctx.real_property_by_blocklot || !ctx.selected_owners ||
        !ctx.selected_parcel_index_set || !ctx.selected_parcel_indices || !ctx.parcel_selection ||
        !ctx.element_info_state || !ctx.show_selected_parcel_details || !ctx.show_selected_zone_details ||
        !ctx.selected_zone_idx || !ctx.center_lon || !ctx.center_lat || !ctx.zoom || !ctx.layer_heatmap_enabled ||
        !ctx.layer_heatmap_max_zoom || !ctx.layer_parcel_detail_min_zoom || !ctx.layer_heatmap_algo ||
        !ctx.layer_heatmap_percentile_clip || !ctx.layer_choropleth_gamma || !ctx.layer_fill_enabled || !ctx.layer_heatmap_state_changed ||
        !ctx.parcel_vac_notice_by_feature || !ctx.parcel_vac_rehab_by_feature || !ctx.parcel_jurisdiction_filter_state ||
        !ctx.owner_class_overrides || !ctx.owner_class_overrides_loaded || !ctx.owner_class_overrides_dirty ||
        !ctx.owner_aggregates || !ctx.filtered_aggregate_snapshot || !ctx.owner_aggregates_dirty ||
        !ctx.owner_sort_mode || !ctx.owner_sorted_mode || !ctx.owner_class_filter_mode ||
        !ctx.owner_class_assign_mode || !ctx.selected_owner_anchor || !ctx.owner_cached_parcel_size ||
        !ctx.owner_cached_real_property_size || !ctx.prof_owner_ms_last || !ctx.owner_search_query ||
        !ctx.address_locate_status || !ctx.address_locate_matches || !ctx.record_year_hist ||
        !ctx.record_year_hist_plot || !ctx.hist_feature_counts || !ctx.hist_enabled || !ctx.hist_dirty ||
        !ctx.record_year_hist_max_bin || !ctx.record_year_nonzero_min || !ctx.record_year_nonzero_max ||
        !ctx.record_year_nonzero_total || !ctx.selected_record_year || !ctx.selected_record_year_dirty ||
        !ctx.selected_record_year_total || !ctx.selected_record_year_samples || !ctx.vacant_notice_rows_matched_total ||
        !ctx.vacant_rehab_rows_matched_total || !ctx.vacant_parcels_matched_total ||
        !ctx.vacant_parcels_with_geometry_total) {
        return;
    }

    auto clear_parcel_selection = [&]() {
        clearParcelSelection(*ctx.parcel_selection);
    };
    auto select_parcel_idx = [&](size_t idx, bool append_toggle) -> bool {
        if (ctx.parcel_layer_idx < 0 || (size_t)ctx.parcel_layer_idx >= ctx.layers->size()) return false;
        const auto& parcel_layer = (*ctx.layers)[(size_t)ctx.parcel_layer_idx];
        if (!selectParcel(*ctx.parcel_selection, idx, parcel_layer.features.size(), append_toggle)) return false;
        openElementParcelPage(*ctx.element_info_state, idx);
        *ctx.show_selected_zone_details = false;
        *ctx.selected_zone_idx = (size_t)-1;
        return true;
    };

    if (ctx.parcel_layer_idx >= 0 && (size_t)ctx.parcel_layer_idx < ctx.layers->size()) {
        pruneParcelSelection(*ctx.parcel_selection, (*ctx.layers)[(size_t)ctx.parcel_layer_idx].features.size());
    } else {
        clear_parcel_selection();
    }

    auto sync_owner_aggregates_if_visible = [&]() {
        syncOwnerAggregates(OwnerAggregatesContext{
            ctx.root,
            ctx.layers,
            ctx.unified_parcels,
            ctx.parcel_layer_idx,
            ctx.real_property_layer_idx,
            ctx.parcel_vacancy_generation_applied,
            ctx.parcel_tax_generation_applied,
            ctx.selected_owners,
            ctx.owner_class_overrides,
            ctx.owner_class_overrides_loaded,
            ctx.owner_class_overrides_dirty,
            ctx.owner_aggregates,
            ctx.filtered_aggregate_snapshot,
            ctx.owner_aggregates_dirty,
            ctx.owner_sorted_mode,
            ctx.owner_cached_parcel_size,
            ctx.owner_cached_real_property_size,
            ctx.prof_owner_ms_last
        });
    };

    ImGui::SetNextWindowPos(ImVec2(ctx.layout_w - ctx.right_panel_w - ctx.layout_margin, ctx.layout_margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ctx.right_panel_w, ctx.main_panel_h), ImGuiCond_Always);
    ImGui::Begin("Record Filters", nullptr, ImGuiWindowFlags_NoCollapse);
    if (ImGui::BeginTabBar("right_tabs")) {
        drawFiltersTab(FiltersTabContext{
            ctx.root,
            ctx.map_filter_state,
            ctx.layers,
            ctx.zoning_metadata,
            ctx.selected_parcel_index_set,
            ctx.parcel_layer_idx,
            ctx.zoning_layer_idx,
            ctx.show_selected_parcel_details,
            ctx.show_selected_zone_details,
            ctx.selected_zone_idx,
            ctx.center_lon,
            ctx.center_lat,
            ctx.zoom,
            ctx.address_locate_status,
            ctx.address_locate_matches,
            ctx.duckdb_analytics,
            ctx.record_year_hist,
            ctx.record_year_hist_plot,
            ctx.hist_feature_counts,
            ctx.hist_enabled,
            ctx.hist_dirty,
            ctx.record_year_hist_max_bin,
            ctx.record_year_nonzero_min,
            ctx.record_year_nonzero_max,
            ctx.record_year_nonzero_total,
            ctx.selected_record_year,
            ctx.selected_record_year_dirty,
            ctx.selected_record_year_total,
            ctx.selected_record_year_samples,
            clear_parcel_selection,
            select_parcel_idx,
            ctx.real_property_for_parcel
        });
        drawSqlTab(
            *ctx.duckdb_analytics,
            *ctx.layers,
            *ctx.unified_parcels,
            *ctx.map_filter_state,
            *ctx.selected_parcel_indices,
            *ctx.show_selected_parcel_details,
            ctx.parcel_layer_idx,
            ctx.parcel_selection->active_idx,
            *ctx.query_layers);
        drawActiveQueriesTab(ActiveQueriesTabContext{
            ctx.map_filter_state,
            ctx.query_layers,
            &ctx.parcel_jurisdiction_filter_state->result_set,
            &ctx.parcel_jurisdiction_filter_state->status,
            ctx.layers,
            ctx.zoning_metadata,
            ctx.zoning_zone_enabled,
            ctx.layer_fill_enabled,
            ctx.zoning_layer_idx,
            ctx.crime_nibrs_layer_idx
        });
        drawVacancyParcelTab(VacancyParcelTabContext{
            ctx.cached_vac_notice_size,
            ctx.cached_vac_rehab_size,
            ctx.vacant_notice_rows_matched_total->load(std::memory_order_relaxed),
            ctx.vacant_rehab_rows_matched_total->load(std::memory_order_relaxed),
            ctx.vacant_parcels_matched_total->load(std::memory_order_relaxed),
            ctx.vacant_parcels_with_geometry_total->load(std::memory_order_relaxed),
            !ctx.selected_owners->empty(),
            ctx.filtered_aggregate_snapshot
        });

        FeatureFilterContextFactoryInput gradient_filter_input;
        gradient_filter_input.layers = ctx.layers;
        gradient_filter_input.map_filters = ctx.map_filter_state;
        gradient_filter_input.result_set = ctx.parcel_jurisdiction_filter_state->result_set.active
            ? &ctx.parcel_jurisdiction_filter_state->result_set
            : nullptr;
        gradient_filter_input.real_property_by_blocklot = ctx.real_property_by_blocklot;
        gradient_filter_input.parcel_vac_notice_by_feature = ctx.parcel_vac_notice_by_feature;
        gradient_filter_input.parcel_vac_rehab_by_feature = ctx.parcel_vac_rehab_by_feature;
        gradient_filter_input.real_property_layer_idx = ctx.real_property_layer_idx;
        gradient_filter_input.parcel_layer_idx = ctx.parcel_layer_idx;
        gradient_filter_input.crime_nibrs_layer_idx = ctx.crime_nibrs_layer_idx;
        gradient_filter_input.query_layers = ctx.query_layers;
        FeatureFilterContext gradient_filter_ctx = makeFeatureFilterContext(gradient_filter_input);
        auto gradient_feature_passes_filters = [&](size_t layer_idx, size_t feature_idx, const LayerDef::FeatureGeom& fg) -> bool {
            return featurePassesFilters(gradient_filter_ctx, layer_idx, feature_idx, fg);
        };
        drawGradientTab(GradientTabContext{
            ctx.layers,
            ctx.layer_heatmap_enabled,
            ctx.layer_heatmap_max_zoom,
            ctx.layer_parcel_detail_min_zoom,
            ctx.layer_heatmap_algo,
            ctx.layer_heatmap_percentile_clip,
            ctx.layer_choropleth_gamma,
            ctx.layer_heatmap_state_changed,
            ctx.parcel_layer_idx,
            ctx.parcel_parameter_mode,
            ctx.unified_parcels,
            gradient_feature_passes_filters,
            ctx.heatmap_percentile_clip,
            ctx.heatmap_algo,
            *ctx.zoom
        });
        drawElementInfoTab(OwnerInfoTabContext{
            ctx.element_info_state,
            ctx.duckdb_analytics,
            ctx.layers,
            ctx.parcel_layer_idx,
            ctx.unified_parcels,
            ctx.selected_parcel_index_set,
            ctx.selected_parcel_indices,
            *ctx.show_selected_parcel_details,
            ctx.vacant_notice_layer_idx,
            ctx.vacant_rehab_layer_idx,
            ctx.tax_lien_layer_idx,
            ctx.tax_sale_layer_idx,
            ctx.center_lon,
            ctx.center_lat,
            ctx.zoom,
            clear_parcel_selection,
            select_parcel_idx,
            ctx.real_property_for_parcel
        });
        drawOwnersTab(OwnersTabContext{
            ctx.owner_aggregates,
            ctx.selected_owners,
            ctx.owner_class_overrides,
            ctx.filtered_aggregate_snapshot,
            ctx.element_info_state,
            ctx.layers,
            ctx.parcel_layer_idx,
            ctx.real_property_layer_idx,
            ctx.owner_sort_mode,
            ctx.owner_sorted_mode,
            ctx.owner_class_filter_mode,
            ctx.owner_class_assign_mode,
            ctx.selected_owner_anchor,
            ctx.owner_class_overrides_dirty,
            ctx.owner_aggregates_dirty,
            ctx.owner_search_query,
            ctx.owner_search_query_size,
            &ownerClassItems(),
            sync_owner_aggregates_if_visible
        });
        ImGui::EndTabBar();
    }
    ImGui::End();
}
