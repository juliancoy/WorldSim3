#include "active_queries_tab.h"
#include "right_panel.h"

#include "filter_context_builder.h"
#include "filters.h"
#include "filters_tab.h"
#include "gradient_tab.h"
#include "gpu_profiler_tab.h"
#include "imgui.h"
#include "app_settings.h"
#include "owner_info.h"
#include "owners_tab.h"
#include "selection.h"
#include "sql_tab.h"
#include "vacancy_parcel_tab.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace {
std::string trimCopy(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace((unsigned char)value[begin])) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace((unsigned char)value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

std::string hostFromUrl(const std::string& url) {
    const size_t scheme = url.find("://");
    const size_t host_begin = scheme == std::string::npos ? 0 : scheme + 3;
    if (host_begin >= url.size()) return {};
    size_t host_end = url.find_first_of("/?#", host_begin);
    if (host_end == std::string::npos) host_end = url.size();
    return url.substr(host_begin, host_end - host_begin);
}

std::string titleCaseHostLabel(std::string host) {
    if (host.empty()) return host;
    std::replace(host.begin(), host.end(), '-', ' ');
    std::replace(host.begin(), host.end(), '.', ' ');
    bool new_word = true;
    for (char& c : host) {
        if (std::isspace((unsigned char)c)) {
            new_word = true;
            continue;
        }
        c = new_word ? (char)std::toupper((unsigned char)c) : (char)std::tolower((unsigned char)c);
        new_word = false;
    }
    return host;
}

std::string inferAgencyFromUrl(const std::string& url) {
    std::string host = hostFromUrl(url);
    std::string lower = host;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (lower.find("hud") != std::string::npos) return "Housing and Urban Development";
    if (lower.find("planning.maryland.gov") != std::string::npos || lower.find("mdgeodata.md.gov") != std::string::npos) {
        return "Maryland Department of Planning";
    }
    if (lower.find("opendata.maryland.gov") != std::string::npos) return "Maryland Open Data";
    if (lower.find("baltimorecity.gov") != std::string::npos) return "Baltimore City Open Data";
    if (lower.find("baltimorecountymd.gov") != std::string::npos) return "Baltimore County";
    if (lower.find("howardcountymd.gov") != std::string::npos) return "Howard County";
    return titleCaseHostLabel(host);
}

std::string primaryParcelSourceLabel(const RightPanelContext& ctx) {
    if (!ctx.layers || ctx.parcel_layer_idx < 0 || (size_t)ctx.parcel_layer_idx >= ctx.layers->size()) return {};
    const LayerDef& layer = (*ctx.layers)[(size_t)ctx.parcel_layer_idx];
    for (const std::string& url : layer.source_urls) {
        if (const std::string label = inferAgencyFromUrl(url); !label.empty()) return label;
    }
    if (const std::string label = inferAgencyFromUrl(layer.source_url); !label.empty()) return label;
    if (const std::string label = inferAgencyFromUrl(layer.reference_url); !label.empty()) return label;
    if (const std::string label = inferAgencyFromUrl(layer.import_url); !label.empty()) return label;
    return trimCopy(layer.name);
}

void drawMapTitleTab(const RightPanelContext& ctx) {
    if (!ImGui::BeginTabItem("Title")) return;
    if (ctx.app_settings && ctx.root) {
        char title_buffer[256];
        std::snprintf(title_buffer, sizeof(title_buffer), "%s", ctx.app_settings->map_title_text.c_str());
        if (ImGui::InputText("Centered map title", title_buffer, sizeof(title_buffer))) {
            ctx.app_settings->map_title_text = trimCopy(title_buffer);
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        ImGui::TextDisabled("Leave the title blank to hide the overlay.");
        bool show_source = ctx.app_settings->map_title_show_primary_parcel_source;
        if (ImGui::Checkbox("Show primary parcel source", &show_source)) {
            ctx.app_settings->map_title_show_primary_parcel_source = show_source;
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        bool all_caps = ctx.app_settings->map_title_all_caps;
        if (ImGui::Checkbox("All caps", &all_caps)) {
            ctx.app_settings->map_title_all_caps = all_caps;
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        bool show_legend = ctx.app_settings->map_legend_show_overlay;
        if (ImGui::Checkbox("Show legend on map", &show_legend)) {
            ctx.app_settings->map_legend_show_overlay = show_legend;
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        static const char* kLegendPositions[] = {"Top Left", "Top Right", "Bottom Left", "Bottom Right"};
        int legend_position = std::clamp(ctx.app_settings->map_legend_overlay_position, 0, 3);
        ImGui::BeginDisabled(!ctx.app_settings->map_legend_show_overlay);
        if (ImGui::Combo("Legend Position", &legend_position, kLegendPositions, IM_ARRAYSIZE(kLegendPositions))) {
            ctx.app_settings->map_legend_overlay_position = legend_position;
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        ImGui::EndDisabled();
        const size_t active_legend_items = ctx.query_layers
            ? std::count_if(
                ctx.query_layers->begin(),
                ctx.query_layers->end(),
                [](const QueryMapLayer& layer) { return layer.enabled; })
            : 0;
        ImGui::TextDisabled(
            "Legend source: %zu active query layer%s",
            active_legend_items,
            active_legend_items == 1 ? "" : "s");
        const std::string preview_source = primaryParcelSourceLabel(ctx);
        if (!preview_source.empty()) {
            ImGui::TextDisabled("Preview: Source: %s", preview_source.c_str());
        } else {
            ImGui::TextDisabled("Preview: no primary parcel source detected");
        }
    }
    ImGui::EndTabItem();
}
}

void drawRightPanelWindow(const RightPanelContext& ctx) {
    if (!ctx.root || !ctx.app_settings || !ctx.duckdb_analytics || !ctx.layers || !ctx.unified_parcels || !ctx.map_filter_state ||
        !ctx.query_layers || !ctx.query_history || !ctx.zoning_metadata || !ctx.zoning_zone_enabled || !ctx.real_property_by_blocklot || !ctx.selected_owners ||
        !ctx.selected_parcel_id_set || !ctx.selected_parcel_ids || !ctx.parcel_selection ||
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
    auto select_parcel_id = [&](const std::string& entity_id, bool append_toggle) -> bool {
        if (entity_id.empty()) return false;
        if (ctx.parcel_layer_idx < 0 || (size_t)ctx.parcel_layer_idx >= ctx.layers->size()) return false;
        const UnifiedParcelRecord* rec = ctx.unified_parcels
            ? unifiedParcelAt(*ctx.unified_parcels, entity_id)
            : nullptr;
        const std::string geometry_entity_id =
            rec && !rec->parcel_geometry_entity_id.empty()
                ? rec->parcel_geometry_entity_id
                : entity_id;
        if (!selectParcel(
                *ctx.parcel_selection,
                ctx.parcel_layer_idx,
                geometry_entity_id,
                entity_id,
                append_toggle)) {
            return false;
        }
        openElementParcelPage(*ctx.element_info_state, entity_id);
        *ctx.show_selected_zone_details = false;
        *ctx.selected_zone_idx = (size_t)-1;
        return true;
    };

    if (ctx.layers) {
        reconcileParcelSelection(*ctx.parcel_selection, *ctx.layers);
    } else {
        clear_parcel_selection();
    }

    auto sync_owner_aggregates_if_visible = [&]() {
        syncOwnerAggregates(OwnerAggregatesContext{
            ctx.root,
            ctx.layers,
            ctx.unified_parcels,
            ctx.parcel_render_blob,
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
            ctx.unified_parcels,
            ctx.zoning_metadata,
            ctx.selected_parcel_id_set,
            ctx.real_property_by_blocklot,
            ctx.parcel_layer_idx,
            ctx.real_property_layer_idx,
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
            select_parcel_id
        });
        drawSqlTab(
            *ctx.duckdb_analytics,
            *ctx.layers,
            *ctx.unified_parcels,
            *ctx.map_filter_state,
            *ctx.app_settings,
            *ctx.root,
            *ctx.center_lon,
            *ctx.center_lat,
            *ctx.zoom,
            *ctx.parcel_selection,
            *ctx.query_layers,
            *ctx.query_history);
        ActiveQueriesTabContext active_queries_ctx{
            ctx.map_filter_state,
            ctx.app_settings,
            ctx.root,
            ctx.query_layers,
            ctx.query_history,
            ctx.duckdb_analytics,
            &ctx.parcel_jurisdiction_filter_state->result_set,
            &ctx.parcel_jurisdiction_filter_state->status,
            ctx.layers,
            ctx.zoning_metadata,
            ctx.zoning_zone_enabled,
            ctx.layer_fill_enabled,
            ctx.center_lon,
            ctx.center_lat,
            ctx.zoom,
            ctx.zoning_layer_idx,
            ctx.crime_nibrs_layer_idx
        };
        drawActiveQueriesTab(active_queries_ctx);
        drawQueryHistoryTab(active_queries_ctx);
        drawGpuProfilerTab(GpuProfilerTabContext{
            ctx.profile_mutex,
            ctx.profile_samples,
            ctx.profile_sample_pos,
            ctx.profile_sample_count,
            ctx.prof_heatmap_gpu_splat_active,
            ctx.prof_heatmap_high_quality,
            ctx.prof_heatmap_texture_resident,
            ctx.prof_heatmap_async_inflight,
            ctx.prof_heatmap_texture_cache_entries,
            ctx.gpu_profiler_tab_requested,
            ctx.gpu_profiler_reload_requested
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
        gradient_filter_input.secondary_result_set = ctx.owner_text_filter_result_set && ctx.owner_text_filter_result_set->active
            ? ctx.owner_text_filter_result_set
            : nullptr;
        gradient_filter_input.tertiary_result_set = ctx.address_text_filter_result_set && ctx.address_text_filter_result_set->active
            ? ctx.address_text_filter_result_set
            : nullptr;
        gradient_filter_input.real_property_by_blocklot = ctx.real_property_by_blocklot;
        gradient_filter_input.compiled_owner_filter_active = gradient_filter_input.secondary_result_set != nullptr;
        gradient_filter_input.compiled_address_filter_active = gradient_filter_input.tertiary_result_set != nullptr;
        gradient_filter_input.parcel_vac_notice_by_feature = ctx.parcel_vac_notice_by_feature;
        gradient_filter_input.parcel_vac_rehab_by_feature = ctx.parcel_vac_rehab_by_feature;
        gradient_filter_input.real_property_layer_idx = ctx.real_property_layer_idx;
        gradient_filter_input.parcel_layer_idx = ctx.parcel_layer_idx;
        gradient_filter_input.crime_nibrs_layer_idx = ctx.crime_nibrs_layer_idx;
        gradient_filter_input.query_layers = ctx.query_layers;
        FeatureFilterContext gradient_filter_ctx = makeFeatureFilterContext(gradient_filter_input);
        auto gradient_feature_passes_filters = [&](size_t layer_idx, size_t feature_idx, const LayerDef::FeatureRecord& fg) -> bool {
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
            ctx.real_property_layer_idx,
            ctx.unified_parcels,
            ctx.real_property_by_blocklot,
            ctx.selected_parcel_id_set,
            ctx.selected_parcel_ids,
            *ctx.show_selected_parcel_details,
            ctx.vacant_notice_layer_idx,
            ctx.vacant_rehab_layer_idx,
            ctx.tax_lien_layer_idx,
            ctx.tax_sale_layer_idx,
            ctx.center_lon,
            ctx.center_lat,
            ctx.zoom,
            ctx.min_zoom,
            ctx.max_zoom,
            ctx.map_w,
            ctx.main_panel_h,
            clear_parcel_selection,
            select_parcel_id
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
        drawMapTitleTab(ctx);
        ImGui::EndTabBar();
    }
    ImGui::End();
}
