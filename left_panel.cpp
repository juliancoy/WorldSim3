#include "left_panel.h"

#include "app_settings.h"
#include "app_utils.h"
#include "basemap_panel.h"
#include "data_library_panel.h"
#include "dataset_library.h"
#include "event_sector_filters_panel.h"
#include "event_sectors.h"
#include "imgui.h"
#include "layer_import.h"
#include "layer_state_io.h"
#include "layer_ui_context_builders.h"
#include "layers_panel_ui.h"
#include "worldsim_app.h"
#include "worldsim_app_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <unordered_map>

namespace {
struct GeographyPreset {
    const char* nation_code;
    const char* nation_label;
    const char* region_code;
    const char* region_label;
};

constexpr std::array<GeographyPreset, 2> kGeographyPresets{{
    {"us", "USA", "md", "Maryland"},
    {"ng", "Nigeria", "anambra", "Anambra"},
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
};

GeographyHierarchyOptions buildGeographyHierarchyOptionsUncached(const std::filesystem::path& root) {
    GeographyHierarchyOptions out;
    std::set<std::string> nations_seen;
    std::unordered_map<std::string, std::set<std::string>> region_sets;

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
    return out;
}

GeographyHierarchyOptions buildGeographyHierarchyOptions(const std::filesystem::path& root) {
    static GeographyHierarchyOptions cached;
    static std::filesystem::path cached_root;
    static std::chrono::steady_clock::time_point cached_at{};
    static std::mutex cache_mutex;
    static std::future<GeographyHierarchyOptions> refresh_future;
    static std::filesystem::path refresh_root;
    static bool refresh_inflight = false;
    constexpr auto kRefreshInterval = std::chrono::seconds(2);

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(cache_mutex);
    if (refresh_inflight &&
        refresh_future.valid() &&
        refresh_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        cached = refresh_future.get();
        cached_root = refresh_root;
        cached_at = now;
        refresh_inflight = false;
    }

    const bool has_cache = !cached_root.empty();
    if (has_cache && cached_root == root && now - cached_at < kRefreshInterval) return cached;

    if (!has_cache) {
        cached = buildGeographyHierarchyOptionsUncached(root);
        cached_root = root;
        cached_at = now;
        return cached;
    }

    if (!refresh_inflight) {
        refresh_root = root;
        refresh_inflight = true;
        refresh_future = std::async(std::launch::async, [root]() {
            return buildGeographyHierarchyOptionsUncached(root);
        });
    }
    return cached;
}

bool layerVisibleInSelectedHierarchy(const LeftPanelContext& ctx, const LayerDef& layer) {
    return !ctx.layer_browse_state || layerMatchesBrowseGeography(layer, *ctx.layer_browse_state);
}

void persistLayerBrowseState(const LeftPanelContext& ctx) {
    if (!ctx.root || !ctx.layer_browse_state) return;
    saveLayerBrowseUiState(
        *ctx.root,
        &ctx.layer_browse_state->selected_nation_state,
        &ctx.layer_browse_state->selected_state_region);
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
        ctx.active_hover_layer_idx && ctx.active_click_layer_idx && ctx.show_sources_panel &&
        ctx.show_data_library && ctx.collapsed && ctx.parcel_parameter_mode && ctx.layer_spatial &&
        ctx.layer_states && ctx.status_mutex && ctx.layer_fill_enabled &&
        ctx.layer_hover_enabled && ctx.layer_inspect_enabled && ctx.layer_heatmap_enabled &&
        ctx.layer_heatmap_algo && ctx.layer_heatmap_max_zoom && ctx.layer_parcel_detail_min_zoom &&
        ctx.layer_normalize_mode && ctx.layer_heatmap_cell_px && ctx.layer_heatmap_bandwidth_px &&
        ctx.layer_heatmap_blur_sigma_px && ctx.layer_heatmap_percentile_clip &&
        ctx.layer_heatmap_multires_blend && ctx.layer_heatmap_zoom_adaptive_bandwidth &&
        ctx.layer_heatmap_multires_enabled && ctx.layer_heatmap_use_gradient &&
        ctx.layer_fill_mutex &&
        ctx.layer_fill_state_changed && ctx.layer_hover_state_changed &&
        ctx.layer_inspect_state_changed && ctx.layer_heatmap_state_changed &&
        ctx.heatmap_controls_active && ctx.crime_filter_enabled && ctx.crime_filter_use_year &&
        ctx.crime_year_min && ctx.crime_year_max && ctx.crime_filter_homicide &&
        ctx.crime_filter_robbery && ctx.crime_filter_assault && ctx.crime_filter_burglary &&
        ctx.crime_filter_theft && ctx.crime_filter_auto_theft && ctx.crime_filter_drug &&
	        ctx.crime_filter_shooting && ctx.crime_breakdown && ctx.parcel_jurisdiction_filter_state &&
        ctx.parcel_jurisdiction_options && ctx.basemap_download && ctx.lazy_tile_download && ctx.map_filter_state &&
        ctx.layer_browse_state &&
        ctx.basemap_coverage_dirty && ctx.road_label_state;
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
    static char layer_search_query[128] = "";

    ImGui::SetNextWindowPos(ImVec2(ctx.layout_margin, ctx.layout_margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ctx.left_panel_w, ctx.main_panel_h), ImGuiCond_Always);
    ImGui::Begin("Layers and Controls", nullptr, ImGuiWindowFlags_NoCollapse);
    if (ImGui::SmallButton("<")) *ctx.collapsed = true;
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Hide left panel");
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    if (ImGui::Button("Gear")) *ctx.show_sources_panel = !*ctx.show_sources_panel;
    ImGui::SameLine();
    if (ImGui::Button("Library")) *ctx.show_data_library = !*ctx.show_data_library;
    ImGui::SameLine();
    ImGui::Text("Vulkan map + Vulkan UI");

    const GeographyHierarchyOptions geography_options = buildGeographyHierarchyOptions(*ctx.root);
    auto& selected_nation = ctx.layer_browse_state->selected_nation_state;
    auto& selected_region = ctx.layer_browse_state->selected_state_region;
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

    auto apply_geography_selection = [&]() {
        persistLayerBrowseState(ctx);
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
        apply_geography_selection();
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
        apply_geography_selection();
    }
    ImGui::EndDisabled();
    if (!selected_nation.empty() || !selected_region.empty()) {
        ImGui::TextDisabled(
            "Active hierarchy: %s / %s",
            geographyNationLabel(selected_nation).c_str(),
            geographyRegionLabel(selected_region).c_str());
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
    double zoom_ui = *ctx.zoom;
    double zoom_min = (double)ctx.min_zoom;
    double zoom_max = (double)ctx.max_zoom;
    if (ImGui::SliderScalar("Zoom", ImGuiDataType_Double, &zoom_ui, &zoom_min, &zoom_max, "%.2f")) {
        *ctx.zoom = std::clamp(zoom_ui, (double)ctx.min_zoom, (double)ctx.max_zoom);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("-")) {
        *ctx.zoom = std::max((double)ctx.min_zoom, *ctx.zoom - std::clamp(ctx.app_settings->zoom_step, 0.05, 4.0));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+")) {
        *ctx.zoom = std::min((double)ctx.max_zoom, *ctx.zoom + std::clamp(ctx.app_settings->zoom_step, 0.05, 4.0));
    }
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
            "No runtime layers are registered for %s / %s yet.",
            geographyNationLabel(selected_nation).c_str(),
            geographyRegionLabel(selected_region).c_str());
    }

    ImGui::SeparatorText("Layer Search");
    ImGui::TextDisabled("Hover target is selected from each layer row.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##layer_search_query", "Filter layers...", layer_search_query, IM_ARRAYSIZE(layer_search_query));

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
    layer_ui_input.layer_download_pending = ctx.layer_download_pending;
    layer_ui_input.mark_local_layer_exists = ctx.mark_local_layer_exists;
    layer_ui_input.enqueue_hydration = ctx.enqueue_hydration;
    layer_ui_input.open_layer_color_editor = ctx.open_layer_color_editor;
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
    layer_ui_input.layer_choropleth_gamma = ctx.layer_choropleth_gamma;
    layer_ui_input.layer_heatmap_multires_blend = ctx.layer_heatmap_multires_blend;
    layer_ui_input.layer_heatmap_zoom_adaptive_bandwidth = ctx.layer_heatmap_zoom_adaptive_bandwidth;
    layer_ui_input.layer_heatmap_multires_enabled = ctx.layer_heatmap_multires_enabled;
    layer_ui_input.layer_heatmap_use_gradient = ctx.layer_heatmap_use_gradient;
    layer_ui_input.heatmap_algo = ctx.heatmap_algo;
    layer_ui_input.layer_fill_mutex = ctx.layer_fill_mutex;
    layer_ui_input.layer_fill_state_changed = ctx.layer_fill_state_changed;
    layer_ui_input.layer_hover_state_changed = ctx.layer_hover_state_changed;
    layer_ui_input.layer_inspect_state_changed = ctx.layer_inspect_state_changed;
    layer_ui_input.layer_heatmap_state_changed = ctx.layer_heatmap_state_changed;
    layer_ui_input.heatmap_controls_active = ctx.heatmap_controls_active;
    layer_ui_input.map_filter_state = ctx.map_filter_state;
    layer_ui_input.layer_browse_state = ctx.layer_browse_state;
    layer_ui_input.road_label_state = ctx.road_label_state;
    LayerUiSharedContext layer_ui_shared = makeLayerUiSharedContext(layer_ui_input);

    LayersPanelContextFactoryInput layers_panel_input;
    layers_panel_input.shared = &layer_ui_shared;
    layers_panel_input.parcel_layer_idx = ctx.parcel_layer_idx;
    layers_panel_input.zoning_layer_idx = ctx.zoning_layer_idx;
    layers_panel_input.zoom = *ctx.zoom;
    layers_panel_input.layer_search_query = layer_search_query;
    layers_panel_input.active_hover_layer_idx = ctx.active_hover_layer_idx;
    layers_panel_input.active_click_layer_idx = ctx.active_click_layer_idx;
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
    layers_panel_input.layer_browse_state = ctx.layer_browse_state;
    LayersPanelUiContext layers_panel_ctx = makeLayersPanelUiContext(layers_panel_input);
    drawLayerCategoriesPanel(layers_panel_ctx);

    if (ctx.map_filter_state) ensureCommunitySectorFilterDefaults(ctx.map_filter_state->event_sector_enabled);

    EventSectorFiltersPanelContext event_sector_filters_ctx;
    event_sector_filters_ctx.layers = ctx.layers;
    event_sector_filters_ctx.map_filter_state = ctx.map_filter_state;
    result.event_sector_filters_changed = drawEventSectorFiltersPanel(event_sector_filters_ctx);
    ImGui::End();
    return result;
}
