#include "left_panel.h"

#include "app_settings.h"
#include "app_utils.h"
#include "basemap_panel.h"
#include "data_library_panel.h"
#include "dataset_library.h"
#include "imgui.h"
#include "layer_import.h"
#include "layer_state_io.h"
#include "layer_ui_context_builders.h"
#include "layers_panel_ui.h"
#include "worldsim_app.h"
#include "worldsim_app_internal.h"
#include "zoning_filters_panel.h"

#include <algorithm>
#include <array>
#include <set>
#include <unordered_map>

namespace {
struct GeographyPreset {
    const char* nation_code;
    const char* nation_label;
    const char* region_code;
    const char* region_label;
    double center_lon;
    double center_lat;
    int suggested_zoom;
};

constexpr std::array<GeographyPreset, 2> kGeographyPresets{{
    {"us", "USA", "md", "Maryland", -76.61, 39.29, 11},
    {"ng", "Nigeria", "anambra", "Anambra", 7.02, 6.17, 10},
}};

std::string geographyNationLabel(std::string_view code) {
    for (const auto& preset : kGeographyPresets) {
        if (code == preset.nation_code) return preset.nation_label;
    }
    return std::string(code);
}

std::string geographyRegionLabel(std::string_view code) {
    for (const auto& preset : kGeographyPresets) {
        if (code == preset.region_code) return preset.region_label;
    }
    return std::string(code);
}

const GeographyPreset* presetForGeography(std::string_view nation_code, std::string_view region_code) {
    for (const auto& preset : kGeographyPresets) {
        if (nation_code == preset.nation_code && region_code == preset.region_code) return &preset;
    }
    return nullptr;
}

struct GeographyHierarchyOptions {
    std::vector<std::string> nation_codes;
    std::unordered_map<std::string, std::vector<std::string>> regions_by_nation;
    std::unordered_map<std::string, std::vector<std::string>> county_cities_by_nation_region;
};

GeographyHierarchyOptions buildGeographyHierarchyOptions(const std::filesystem::path& root) {
    GeographyHierarchyOptions out;
    std::set<std::string> nations_seen;
    std::unordered_map<std::string, std::set<std::string>> region_sets;
    std::unordered_map<std::string, std::set<std::string>> county_city_sets;

    const std::filesystem::path nation_root = root / "sources" / "world" / "earth" / "nation_state";
    std::error_code ec;
    if (std::filesystem::exists(nation_root, ec) && !ec) {
        for (const auto& nation_entry : std::filesystem::directory_iterator(nation_root, ec)) {
            if (ec) break;
            if (!nation_entry.is_directory()) continue;
            const std::string nation = toLowerAscii(trimDisplayValue(nation_entry.path().filename().string()));
            if (nation.empty()) continue;
            nations_seen.insert(nation);

            const std::filesystem::path region_root = nation_entry.path() / "state_region";
            std::error_code region_ec;
            if (!std::filesystem::exists(region_root, region_ec) || region_ec) continue;
            for (const auto& region_entry : std::filesystem::directory_iterator(region_root, region_ec)) {
                if (region_ec) break;
                if (!region_entry.is_directory()) continue;
                const std::string region = toLowerAscii(trimDisplayValue(region_entry.path().filename().string()));
                if (region.empty()) continue;
                region_sets[nation].insert(region);

                const std::filesystem::path county_city_root = region_entry.path() / "county_city";
                std::error_code county_city_ec;
                if (!std::filesystem::exists(county_city_root, county_city_ec) || county_city_ec) continue;
                const std::string geography_key = nation + "/" + region;
                for (const auto& county_city_entry : std::filesystem::directory_iterator(county_city_root, county_city_ec)) {
                    if (county_city_ec) break;
                    if (!county_city_entry.is_directory()) continue;
                    const std::string county_city = toLowerAscii(trimDisplayValue(county_city_entry.path().filename().string()));
                    if (county_city.empty()) continue;
                    county_city_sets[geography_key].insert(county_city);
                }
            }
        }
    }

    for (const auto& preset : kGeographyPresets) {
        if (nations_seen.contains(preset.nation_code)) {
            out.nation_codes.push_back(preset.nation_code);
        }
        if (region_sets.contains(preset.nation_code) && region_sets[preset.nation_code].contains(preset.region_code)) {
            out.regions_by_nation[preset.nation_code].push_back(preset.region_code);
        }
    }
    for (const auto& nation : nations_seen) {
        if (std::find(out.nation_codes.begin(), out.nation_codes.end(), nation) == out.nation_codes.end()) {
            out.nation_codes.push_back(nation);
        }
        auto& out_regions = out.regions_by_nation[nation];
        for (const auto& region : region_sets[nation]) {
            if (std::find(out_regions.begin(), out_regions.end(), region) == out_regions.end()) out_regions.push_back(region);
        }
    }
    for (const auto& [geography_key, county_city_set] : county_city_sets) {
        auto& out_county_cities = out.county_cities_by_nation_region[geography_key];
        out_county_cities.assign(county_city_set.begin(), county_city_set.end());
    }
    return out;
}

bool layerVisibleInSelectedHierarchy(const LeftPanelContext& ctx, const LayerDef& layer) {
    return !ctx.map_filter_state || layerMatchesSelectedGeography(layer, *ctx.map_filter_state);
}

void persistGeographyFilterState(const LeftPanelContext& ctx) {
    if (!ctx.root || !ctx.map_filter_state) return;
    saveFilterUiState(
        *ctx.root,
        &ctx.map_filter_state->selected_nation_state,
        &ctx.map_filter_state->selected_state_region,
        &ctx.map_filter_state->selected_county_city,
        ctx.map_filter_state->enabled,
        ctx.map_filter_state->use_date,
        ctx.map_filter_state->year_min,
        ctx.map_filter_state->year_max,
        ctx.map_filter_state->blocklot,
        ctx.map_filter_state->status,
        ctx.map_filter_state->address,
        ctx.map_filter_state->owner,
        ctx.map_filter_state->zip,
        ctx.map_filter_state->crime.enabled,
        ctx.map_filter_state->crime.homicide,
        ctx.map_filter_state->crime.robbery,
        ctx.map_filter_state->crime.assault,
        ctx.map_filter_state->crime.burglary,
        ctx.map_filter_state->crime.theft,
        ctx.map_filter_state->crime.auto_theft,
        ctx.map_filter_state->crime.drug,
        ctx.map_filter_state->crime.shooting,
        ctx.map_filter_state->crime.use_year,
        ctx.map_filter_state->crime.year_min,
        ctx.map_filter_state->crime.year_max,
        nullptr,
        ctx.map_filter_state->selected_owners);
}

bool heatmapInputFloatEnter(
    bool& heatmap_controls_active,
    const char* label,
    float& value,
    float min_value,
    float max_value,
    const char* format) {
    static std::unordered_map<ImGuiID, float> drafts;
    static std::unordered_map<ImGuiID, bool> was_active;
    const ImGuiID id = ImGui::GetID(label);
    if (!was_active[id]) drafts[id] = value;
    float draft = drafts[id];
    const bool committed = ImGui::InputFloat(label, &draft, 0.0f, 0.0f, format, ImGuiInputTextFlags_EnterReturnsTrue);
    const bool active = ImGui::IsItemActive();
    heatmap_controls_active |= active;
    if (committed) {
        value = std::clamp(draft, min_value, max_value);
        drafts[id] = value;
        was_active[id] = active;
        return true;
    }
    drafts[id] = active ? draft : value;
    was_active[id] = active;
    return false;
}

bool leftPanelContextReady(const LeftPanelContext& ctx) {
    return ctx.root && ctx.app_settings && ctx.layers && ctx.layer_registry &&
        ctx.local_layer_exists_cache && ctx.data_freshness_state && ctx.data_freshness_msg &&
        ctx.data_library_status_msg && ctx.zoom && ctx.center_lon && ctx.center_lat &&
        ctx.hover_inspector_mode && ctx.hover_inspector_enabled && ctx.show_sources_panel &&
        ctx.show_data_library && ctx.parcel_parameter_mode && ctx.layer_spatial &&
        ctx.layer_states && ctx.status_mutex && ctx.layer_fill_enabled &&
        ctx.layer_hover_enabled && ctx.layer_inspect_enabled && ctx.layer_heatmap_enabled &&
        ctx.layer_heatmap_algo && ctx.layer_heatmap_max_zoom && ctx.layer_parcel_detail_min_zoom &&
        ctx.layer_normalize_mode && ctx.layer_heatmap_cell_px && ctx.layer_heatmap_bandwidth_px &&
        ctx.layer_heatmap_blur_sigma_px && ctx.layer_heatmap_percentile_clip &&
        ctx.layer_heatmap_multires_blend && ctx.layer_heatmap_zoom_adaptive_bandwidth &&
        ctx.layer_heatmap_multires_enabled && ctx.layer_heatmap_use_gradient &&
        ctx.heatmap_allow_cpu_fallback && ctx.layer_fill_mutex &&
        ctx.layer_fill_state_changed && ctx.layer_hover_state_changed &&
        ctx.layer_inspect_state_changed && ctx.layer_heatmap_state_changed &&
        ctx.heatmap_controls_active && ctx.crime_filter_enabled && ctx.crime_filter_use_year &&
        ctx.crime_year_min && ctx.crime_year_max && ctx.crime_filter_homicide &&
        ctx.crime_filter_robbery && ctx.crime_filter_assault && ctx.crime_filter_burglary &&
        ctx.crime_filter_theft && ctx.crime_filter_auto_theft && ctx.crime_filter_drug &&
	        ctx.crime_filter_shooting && ctx.crime_breakdown && ctx.parcel_jurisdiction_filter_state &&
        ctx.parcel_jurisdiction_options && ctx.basemap_download && ctx.lazy_tile_download && ctx.map_filter_state &&
	        ctx.basemap_coverage_dirty && ctx.zoning_zone_enabled && ctx.zoning_zone_color &&
	        ctx.zoning_zone_label && ctx.zoning_metadata && ctx.zoning_zone_order &&
	        ctx.zoning_zone_counts && ctx.zoning_group_zones && ctx.zoning_group_order;
}

void selectAllLayersAndFilters(const LeftPanelContext& ctx) {
    for (auto& layer : *ctx.layers) {
        if (!layerVisibleInSelectedHierarchy(ctx, layer)) continue;
        layer.enabled = true;
    }
    ctx.parcel_jurisdiction_filter_state->selected_jurisdictions.clear();
    for (size_t i = 0; i < ctx.parcel_jurisdiction_option_count; ++i) {
        ctx.parcel_jurisdiction_filter_state->selected_jurisdictions.insert(ctx.parcel_jurisdiction_options[i]);
    }
    ctx.parcel_jurisdiction_filter_state->dirty = true;
}

void deselectAllLayersAndFilters(const LeftPanelContext& ctx) {
    for (auto& layer : *ctx.layers) {
        if (!layerVisibleInSelectedHierarchy(ctx, layer)) continue;
        layer.enabled = false;
    }
    ctx.parcel_jurisdiction_filter_state->selected_jurisdictions.clear();
    ctx.parcel_jurisdiction_filter_state->dirty = true;
}
}

LeftPanelResult drawLeftPanelWindow(const LeftPanelContext& ctx) {
    LeftPanelResult result;
    if (!leftPanelContextReady(ctx)) return result;

    ImGui::SetNextWindowPos(ImVec2(ctx.layout_margin, ctx.layout_margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ctx.left_panel_w, ctx.main_panel_h), ImGuiCond_Always);
    ImGui::Begin("Layers and Controls", nullptr, ImGuiWindowFlags_NoCollapse);
    if (ImGui::Button("Gear")) *ctx.show_sources_panel = !*ctx.show_sources_panel;
    ImGui::SameLine();
    if (ImGui::Button("Library")) *ctx.show_data_library = !*ctx.show_data_library;
    ImGui::SameLine();
    ImGui::Text("Vulkan map + Vulkan UI");

    const GeographyHierarchyOptions geography_options = buildGeographyHierarchyOptions(*ctx.root);
    auto& selected_nation = ctx.map_filter_state->selected_nation_state;
    auto& selected_region = ctx.map_filter_state->selected_state_region;
    auto& selected_county_city = ctx.map_filter_state->selected_county_city;
    if (!geography_options.nation_codes.empty() &&
        std::find(geography_options.nation_codes.begin(), geography_options.nation_codes.end(), selected_nation) == geography_options.nation_codes.end()) {
        selected_nation = geography_options.nation_codes.front();
    }
    auto region_it = geography_options.regions_by_nation.find(selected_nation);
    if (region_it == geography_options.regions_by_nation.end() || region_it->second.empty()) {
        selected_region.clear();
    } else if (std::find(region_it->second.begin(), region_it->second.end(), selected_region) == region_it->second.end()) {
        selected_region = region_it->second.front();
    }
    auto county_city_key = selected_nation + "/" + selected_region;
    auto county_city_it = geography_options.county_cities_by_nation_region.find(county_city_key);
    if (county_city_it == geography_options.county_cities_by_nation_region.end() ||
        std::find(county_city_it->second.begin(), county_city_it->second.end(), selected_county_city) == county_city_it->second.end()) {
        selected_county_city.clear();
    }

    auto apply_geography_selection = [&](bool center_map) {
        if (const GeographyPreset* preset = presetForGeography(selected_nation, selected_region)) {
            if (center_map) {
                *ctx.center_lon = preset->center_lon;
                *ctx.center_lat = preset->center_lat;
                *ctx.zoom = std::max(*ctx.zoom, preset->suggested_zoom);
            }
        }
        clearParcelGpuBuffers();
        persistGeographyFilterState(ctx);
        result.geography_changed = true;
    };

    ImGui::SeparatorText("Geography");
    std::vector<std::string> nation_label_storage;
    std::vector<const char*> nation_labels;
    nation_labels.reserve(geography_options.nation_codes.size());
    nation_label_storage.reserve(geography_options.nation_codes.size());
    int nation_idx = 0;
    for (size_t i = 0; i < geography_options.nation_codes.size(); ++i) {
        nation_label_storage.push_back(geographyNationLabel(geography_options.nation_codes[i]));
        nation_labels.push_back(nation_label_storage.back().c_str());
        if (geography_options.nation_codes[i] == selected_nation) nation_idx = (int)i;
    }
    ImGui::TextDisabled("Nation");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!nation_labels.empty() && ImGui::Combo("##geography_nation", &nation_idx, nation_labels.data(), (int)nation_labels.size())) {
        selected_nation = geography_options.nation_codes[(size_t)nation_idx];
        const auto region_match = geography_options.regions_by_nation.find(selected_nation);
        selected_region = (region_match != geography_options.regions_by_nation.end() && !region_match->second.empty())
            ? region_match->second.front()
            : std::string();
        selected_county_city.clear();
        apply_geography_selection(true);
    }
    region_it = geography_options.regions_by_nation.find(selected_nation);
    std::vector<std::string> region_label_storage;
    std::vector<const char*> region_labels;
    int region_idx = 0;
    if (region_it != geography_options.regions_by_nation.end()) {
        region_labels.reserve(region_it->second.size());
        region_label_storage.reserve(region_it->second.size());
        for (size_t i = 0; i < region_it->second.size(); ++i) {
            region_label_storage.push_back(geographyRegionLabel(region_it->second[i]));
            region_labels.push_back(region_label_storage.back().c_str());
            if (region_it->second[i] == selected_region) region_idx = (int)i;
        }
    }
    ImGui::TextDisabled("Region");
    ImGui::BeginDisabled(region_labels.empty());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!region_labels.empty() && ImGui::Combo("##geography_region", &region_idx, region_labels.data(), (int)region_labels.size())) {
        selected_region = region_it->second[(size_t)region_idx];
        selected_county_city.clear();
        apply_geography_selection(true);
    }
    ImGui::EndDisabled();
    county_city_key = selected_nation + "/" + selected_region;
    county_city_it = geography_options.county_cities_by_nation_region.find(county_city_key);
    std::vector<std::string> county_city_label_storage;
    std::vector<const char*> county_city_labels;
    county_city_label_storage.reserve((county_city_it != geography_options.county_cities_by_nation_region.end() ? county_city_it->second.size() : 0) + 1);
    county_city_labels.reserve(county_city_label_storage.capacity());
    county_city_label_storage.push_back("All Areas");
    county_city_labels.push_back(county_city_label_storage.back().c_str());
    int county_city_idx = 0;
    if (county_city_it != geography_options.county_cities_by_nation_region.end()) {
        for (size_t i = 0; i < county_city_it->second.size(); ++i) {
            county_city_label_storage.push_back(county_city_it->second[i]);
            county_city_labels.push_back(county_city_label_storage.back().c_str());
            if (county_city_it->second[i] == selected_county_city) county_city_idx = (int)i + 1;
        }
    }
    ImGui::TextDisabled("Area");
    ImGui::BeginDisabled(county_city_labels.size() <= 1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##geography_area", &county_city_idx, county_city_labels.data(), (int)county_city_labels.size())) {
        selected_county_city = county_city_idx == 0 ? std::string() : county_city_it->second[(size_t)county_city_idx - 1];
        apply_geography_selection(false);
    }
    ImGui::EndDisabled();
    if (!selected_nation.empty() || !selected_region.empty() || !selected_county_city.empty()) {
        ImGui::TextDisabled(
            "Active hierarchy: %s / %s / %s",
            geographyNationLabel(selected_nation).c_str(),
            geographyRegionLabel(selected_region).c_str(),
            selected_county_city.empty() ? "all areas" : selected_county_city.c_str());
    }

    size_t local_layer_count = 0;
    size_t downloadable_missing_layer_count = 0;
    size_t queueable_missing_layer_count = 0;
    for (size_t i = 0; i < ctx.local_layer_exists_cache->size(); ++i) {
        if (i >= ctx.layers->size()) continue;
        if (!layerVisibleInSelectedHierarchy(ctx, (*ctx.layers)[i])) continue;
        if ((*ctx.local_layer_exists_cache)[i]) {
            local_layer_count++;
        } else if (i < ctx.layers->size() &&
                   (!(*ctx.layers)[i].source_url.empty() || layerHasImportSource((*ctx.layers)[i]))) {
            downloadable_missing_layer_count++;
            if (!ctx.layer_download_pending || !ctx.layer_download_pending(i)) queueable_missing_layer_count++;
        }
    }

    size_t visible_layer_total = 0;
    for (const LayerDef& layer : *ctx.layers) {
        if (layerVisibleInSelectedHierarchy(ctx, layer)) visible_layer_total++;
    }
    ImGui::TextDisabled("Local data: %zu/%zu", local_layer_count, visible_layer_total);
    result.downloadable_missing_layer_count = downloadable_missing_layer_count;
    result.queueable_missing_layer_count = queueable_missing_layer_count;
    ImGui::SliderInt("Zoom", ctx.zoom, ctx.min_zoom, ctx.max_zoom);
    double lon_min = -180.0;
    double lon_max = 180.0;
    double lat_min = -85.0;
    double lat_max = 85.0;
    if (selected_nation == "us" && selected_region == "md") {
        lon_min = -79.8;
        lon_max = -74.8;
        lat_min = 37.7;
        lat_max = 40.95;
    } else if (selected_nation == "ng" && selected_region == "anambra") {
        lon_min = 6.3;
        lon_max = 7.7;
        lat_min = 5.7;
        lat_max = 6.8;
    }
    ImGui::SliderScalar("Center Lon", ImGuiDataType_Double, ctx.center_lon, &lon_min, &lon_max, "%.6f");
    ImGui::SliderScalar("Center Lat", ImGuiDataType_Double, ctx.center_lat, &lat_min, &lat_max, "%.6f");
    if (visible_layer_total == 0) {
        ImGui::TextDisabled(
            "No runtime layers are registered for %s / %s / %s yet.",
            geographyNationLabel(selected_nation).c_str(),
            geographyRegionLabel(selected_region).c_str(),
            selected_county_city.empty() ? "all areas" : selected_county_city.c_str());
    }

    const char* hover_inspector_mode_options[] = {"None", "Parcels", "Zoning", "All Supported"};
    if (ImGui::Combo("Hover Inspector", ctx.hover_inspector_mode, hover_inspector_mode_options, IM_ARRAYSIZE(hover_inspector_mode_options))) {
        *ctx.hover_inspector_mode = std::clamp(*ctx.hover_inspector_mode, 0, 3);
        *ctx.hover_inspector_enabled = *ctx.hover_inspector_mode != 0;
    }

    ImGui::SeparatorText("Heatmap");
    ImGui::TextDisabled("Global heatmap controls removed.");
    ImGui::TextDisabled("Use each layer's settings (⚙) to configure aggregate method and heatmap parameters.");

    bool validation_ui = g_EnableValidationLayers;
    if (ImGui::Checkbox("Vulkan Validation (restart required)", &validation_ui)) {
        g_EnableValidationLayers = validation_ui;
        ctx.app_settings->vulkan_validation_enabled = g_EnableValidationLayers;
        saveAppSettings(*ctx.root, *ctx.app_settings);
    }
    ImGui::TextDisabled("Underlying map sources are independent layers below.");

    int reserve_cpu_cores_ui = std::clamp(ctx.app_settings->reserve_cpu_cores, 0, 8);
    if (ImGui::SliderInt("Reserve CPU cores (restart required)", &reserve_cpu_cores_ui, 0, 8)) {
        ctx.app_settings->reserve_cpu_cores = reserve_cpu_cores_ui;
        saveAppSettings(*ctx.root, *ctx.app_settings);
    }

    ImGui::Separator();
    if (ImGui::Button("Show All")) {
        selectAllLayersAndFilters(ctx);
    }
    ImGui::SameLine();
    if (ImGui::Button("Deselect All")) {
        deselectAllLayersAndFilters(ctx);
    }
    ImGui::SameLine();
    if (ImGui::Button("Hide All")) {
        deselectAllLayersAndFilters(ctx);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(queueable_missing_layer_count == 0);
    if (ImGui::Button("Download All")) {
        for (size_t i = 0; i < ctx.layers->size(); ++i) {
            if (!layerVisibleInSelectedHierarchy(ctx, (*ctx.layers)[i])) continue;
            const bool local_exists =
                ctx.local_layer_exists_cache && i < ctx.local_layer_exists_cache->size()
                    ? (*ctx.local_layer_exists_cache)[i]
                    : false;
            if (local_exists) continue;
            const bool can_download = !(*ctx.layers)[i].source_url.empty() || layerHasImportSource((*ctx.layers)[i]);
            if (!can_download || !ctx.enqueue_layer_download_request) continue;
            if (ctx.layer_download_pending && ctx.layer_download_pending(i)) continue;
            ctx.enqueue_layer_download_request(i);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::Text("Queue all missing layer datasets with source URLs");
        ImGui::TextDisabled("Missing downloadable: %zu", downloadable_missing_layer_count);
        ImGui::TextDisabled("Queueable now: %zu", queueable_missing_layer_count);
        ImGui::EndTooltip();
    }

    BasemapPanelContext basemap_panel_ctx;
    basemap_panel_ctx.root = ctx.root;
    basemap_panel_ctx.app_settings = ctx.app_settings;
    basemap_panel_ctx.basemap_download = ctx.basemap_download;
    basemap_panel_ctx.lazy_tile_download = ctx.lazy_tile_download;
    basemap_panel_ctx.data_library_status_msg = ctx.data_library_status_msg;
    basemap_panel_ctx.basemap_coverage_dirty = ctx.basemap_coverage_dirty;
    basemap_panel_ctx.osm_missing_tiles_cached = ctx.osm_missing_tiles_cached;
    basemap_panel_ctx.osm_total_tiles_cached = ctx.osm_total_tiles_cached;
    basemap_panel_ctx.topo_missing_tiles_cached = ctx.topo_missing_tiles_cached;
    basemap_panel_ctx.topo_total_tiles_cached = ctx.topo_total_tiles_cached;
    basemap_panel_ctx.topo_tiles_available_cached = ctx.topo_tiles_available_cached;
    basemap_panel_ctx.topo_vector_available_cached = ctx.topo_vector_available_cached;
    basemap_panel_ctx.min_zoom = ctx.min_zoom;
    basemap_panel_ctx.max_native_tile_zoom = ctx.max_native_tile_zoom;
    basemap_panel_ctx.max_satellite_native_tile_zoom = ctx.max_satellite_native_tile_zoom;
    basemap_panel_ctx.max_night_satellite_native_tile_zoom = ctx.max_night_satellite_native_tile_zoom;
    drawBasemapPanel(basemap_panel_ctx);

    LayerUiContextFactoryInput layer_ui_input;
    layer_ui_input.root = *ctx.root;
    layer_ui_input.layers = ctx.layers;
    layer_ui_input.layer_registry = ctx.layer_registry;
    layer_ui_input.local_layer_exists_cache = ctx.local_layer_exists_cache;
    layer_ui_input.data_freshness_state = ctx.data_freshness_state;
    layer_ui_input.data_freshness_msg = ctx.data_freshness_msg;
    layer_ui_input.data_library_status_msg = ctx.data_library_status_msg;
    layer_ui_input.parcel_parameter_mode = ctx.parcel_parameter_mode;
    layer_ui_input.enqueue_layer_download_request = ctx.enqueue_layer_download_request;
    layer_ui_input.mark_local_layer_exists = ctx.mark_local_layer_exists;
    layer_ui_input.enqueue_hydration = ctx.enqueue_hydration;
    layer_ui_input.heatmap_input_float_enter = [&](const char* label, float& value, float min_value, float max_value, const char* format) {
        return heatmapInputFloatEnter(*ctx.heatmap_controls_active, label, value, min_value, max_value, format);
    };
    layer_ui_input.layer_spatial = ctx.layer_spatial;
    layer_ui_input.layer_states = ctx.layer_states;
    layer_ui_input.status_mutex = ctx.status_mutex;
    layer_ui_input.layer_fill_enabled = ctx.layer_fill_enabled;
    layer_ui_input.layer_hover_enabled = ctx.layer_hover_enabled;
    layer_ui_input.layer_inspect_enabled = ctx.layer_inspect_enabled;
    layer_ui_input.layer_heatmap_enabled = ctx.layer_heatmap_enabled;
    layer_ui_input.layer_heatmap_algo = ctx.layer_heatmap_algo;
    layer_ui_input.layer_heatmap_max_zoom = ctx.layer_heatmap_max_zoom;
    layer_ui_input.layer_parcel_detail_min_zoom = ctx.layer_parcel_detail_min_zoom;
    layer_ui_input.layer_normalize_mode = ctx.layer_normalize_mode;
    layer_ui_input.layer_heatmap_cell_px = ctx.layer_heatmap_cell_px;
    layer_ui_input.layer_heatmap_bandwidth_px = ctx.layer_heatmap_bandwidth_px;
    layer_ui_input.layer_heatmap_blur_sigma_px = ctx.layer_heatmap_blur_sigma_px;
    layer_ui_input.layer_heatmap_percentile_clip = ctx.layer_heatmap_percentile_clip;
    layer_ui_input.layer_heatmap_multires_blend = ctx.layer_heatmap_multires_blend;
    layer_ui_input.layer_heatmap_zoom_adaptive_bandwidth = ctx.layer_heatmap_zoom_adaptive_bandwidth;
    layer_ui_input.layer_heatmap_multires_enabled = ctx.layer_heatmap_multires_enabled;
    layer_ui_input.layer_heatmap_use_gradient = ctx.layer_heatmap_use_gradient;
    layer_ui_input.heatmap_algo = ctx.heatmap_algo;
    layer_ui_input.heatmap_allow_cpu_fallback = ctx.heatmap_allow_cpu_fallback;
    layer_ui_input.layer_fill_mutex = ctx.layer_fill_mutex;
    layer_ui_input.layer_fill_state_changed = ctx.layer_fill_state_changed;
    layer_ui_input.layer_hover_state_changed = ctx.layer_hover_state_changed;
    layer_ui_input.layer_inspect_state_changed = ctx.layer_inspect_state_changed;
    layer_ui_input.layer_heatmap_state_changed = ctx.layer_heatmap_state_changed;
    layer_ui_input.heatmap_controls_active = ctx.heatmap_controls_active;
    layer_ui_input.map_filter_state = ctx.map_filter_state;
    LayerUiSharedContext layer_ui_shared = makeLayerUiSharedContext(layer_ui_input);

    LayersPanelContextFactoryInput layers_panel_input;
    layers_panel_input.shared = &layer_ui_shared;
    layers_panel_input.parcel_layer_idx = ctx.parcel_layer_idx;
    layers_panel_input.zoom = *ctx.zoom;
    layers_panel_input.crime_filter_enabled = ctx.crime_filter_enabled;
    layers_panel_input.crime_filter_use_year = ctx.crime_filter_use_year;
    layers_panel_input.crime_year_min = ctx.crime_year_min;
    layers_panel_input.crime_year_max = ctx.crime_year_max;
    layers_panel_input.crime_filter_homicide = ctx.crime_filter_homicide;
    layers_panel_input.crime_filter_robbery = ctx.crime_filter_robbery;
    layers_panel_input.crime_filter_assault = ctx.crime_filter_assault;
    layers_panel_input.crime_filter_burglary = ctx.crime_filter_burglary;
    layers_panel_input.crime_filter_theft = ctx.crime_filter_theft;
    layers_panel_input.crime_filter_auto_theft = ctx.crime_filter_auto_theft;
    layers_panel_input.crime_filter_drug = ctx.crime_filter_drug;
    layers_panel_input.crime_filter_shooting = ctx.crime_filter_shooting;
    layers_panel_input.crime_nibrs_layer_idx = ctx.crime_nibrs_layer_idx;
    layers_panel_input.crime_breakdown = ctx.crime_breakdown;
    layers_panel_input.parcel_jurisdiction_filter_state = ctx.parcel_jurisdiction_filter_state;
    layers_panel_input.map_filter_state = ctx.map_filter_state;
    LayersPanelUiContext layers_panel_ctx = makeLayersPanelUiContext(layers_panel_input);
    drawLayerCategoriesPanel(layers_panel_ctx);

    ZoningFiltersPanelContext zoning_filters_ctx;
    zoning_filters_ctx.zoning_layer_idx = ctx.zoning_layer_idx;
    zoning_filters_ctx.root = ctx.root;
    zoning_filters_ctx.app_settings = ctx.app_settings;
    zoning_filters_ctx.zoning_zone_enabled = ctx.zoning_zone_enabled;
    zoning_filters_ctx.zoning_zone_color = ctx.zoning_zone_color;
    zoning_filters_ctx.zoning_zone_label = ctx.zoning_zone_label;
    zoning_filters_ctx.zoning_metadata = ctx.zoning_metadata;
    zoning_filters_ctx.zoning_zone_order = ctx.zoning_zone_order;
    zoning_filters_ctx.zoning_zone_counts = ctx.zoning_zone_counts;
    zoning_filters_ctx.zoning_group_zones = ctx.zoning_group_zones;
    zoning_filters_ctx.zoning_group_order = ctx.zoning_group_order;
    result.zoning_filters_changed = drawZoningFiltersPanel(zoning_filters_ctx);
    ImGui::End();
    return result;
}
