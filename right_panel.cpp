#include "active_queries_tab.h"
#include "right_panel.h"

#include "av_capture.h"
#include "filter_context_builder.h"
#include "filters.h"
#include "filters_tab.h"
#include "gradient_tab.h"
#include "gpu_profiler_tab.h"
#include "imgui.h"
#include "app_settings.h"
#include "map_title_source.h"
#include "owner_info.h"
#include "owners_tab.h"
#include "project_state.h"
#include "selection.h"
#include "sql_tab.h"
#include "vacancy_parcel_tab.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <type_traits>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {
std::string trimCopy(const std::string& value);

std::string trimCopy(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace((unsigned char)value[begin])) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace((unsigned char)value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

json colorJson(const float c[4]) {
    return json::array({c[0], c[1], c[2], c[3]});
}

json colorJson(const ImVec4& c) {
    return json::array({c.x, c.y, c.z, c.w});
}

void loadColorJson(const json& j, float c[4]) {
    if (!j.is_array()) return;
    for (int i = 0; i < 4 && i < (int)j.size(); ++i) {
        if (j[(size_t)i].is_number()) c[i] = j[(size_t)i].get<float>();
    }
}

void loadColorJson(const json& j, ImVec4& c) {
    float tmp[4] = {c.x, c.y, c.z, c.w};
    loadColorJson(j, tmp);
    c = ImVec4(tmp[0], tmp[1], tmp[2], tmp[3]);
}

void copyProjectText(const json& j, const char* key, char* dst, size_t dst_size) {
    if (!dst || dst_size == 0 || !j.contains(key) || !j[key].is_string()) return;
    std::strncpy(dst, j[key].get<std::string>().c_str(), dst_size - 1);
    dst[dst_size - 1] = '\0';
}

json stringSetJson(const std::unordered_set<std::string>& values) {
    std::vector<std::string> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

json stringBoolMapJson(const std::unordered_map<std::string, bool>& values) {
    json out = json::object();
    for (const auto& kv : values) out[kv.first] = kv.second;
    return out;
}

void loadStringSetJson(const json& j, std::unordered_set<std::string>& out) {
    if (!j.is_array()) return;
    out.clear();
    for (const auto& item : j) if (item.is_string()) out.insert(item.get<std::string>());
}

void loadStringBoolMapJson(const json& j, std::unordered_map<std::string, bool>& out) {
    if (!j.is_object()) return;
    out.clear();
    for (auto it = j.begin(); it != j.end(); ++it) if (it.value().is_boolean()) out[it.key()] = it.value().get<bool>();
}

json filterResultSetJson(const FilterResultSet& set) {
    json features = json::array();
    for (const FeatureKey& key : set.features) features.push_back({{"layer_idx", key.layer_idx}, {"feature_idx", key.feature_idx}});
    json layers = json::array();
    for (size_t layer_idx : set.layers) layers.push_back(layer_idx);
    return {
        {"active", set.active},
        {"layers", std::move(layers)},
        {"features", std::move(features)},
        {"blocklots", stringSetJson(set.blocklots)},
        {"owners", stringSetJson(set.owners)}
    };
}

void loadFilterResultSetJson(const json& j, FilterResultSet& set) {
    if (!j.is_object()) return;
    set = FilterResultSet{};
    if (j.contains("active") && j["active"].is_boolean()) set.active = j["active"].get<bool>();
    if (j.contains("layers") && j["layers"].is_array()) {
        for (const auto& item : j["layers"]) if (item.is_number_unsigned()) set.layers.insert(item.get<size_t>());
    }
    if (j.contains("features") && j["features"].is_array()) {
        for (const auto& item : j["features"]) {
            if (!item.is_object() || !item.contains("layer_idx") || !item.contains("feature_idx")) continue;
            if (item["layer_idx"].is_number_unsigned() && item["feature_idx"].is_number_unsigned()) {
                set.features.insert(FeatureKey{item["layer_idx"].get<size_t>(), item["feature_idx"].get<size_t>()});
            }
        }
    }
    if (j.contains("blocklots")) loadStringSetJson(j["blocklots"], set.blocklots);
    if (j.contains("owners")) loadStringSetJson(j["owners"], set.owners);
}

json crimeFilterJson(const CrimeFilterState& crime) {
    return {
        {"enabled", crime.enabled},
        {"homicide", crime.homicide},
        {"robbery", crime.robbery},
        {"assault", crime.assault},
        {"burglary", crime.burglary},
        {"theft", crime.theft},
        {"auto_theft", crime.auto_theft},
        {"drug", crime.drug},
        {"shooting", crime.shooting},
        {"use_year", crime.use_year},
        {"year_min", crime.year_min},
        {"year_max", crime.year_max}
    };
}

void loadCrimeFilterJson(const json& j, CrimeFilterState& crime) {
    if (!j.is_object()) return;
    if (j.contains("enabled") && j["enabled"].is_boolean()) crime.enabled = j["enabled"].get<bool>();
    if (j.contains("homicide") && j["homicide"].is_boolean()) crime.homicide = j["homicide"].get<bool>();
    if (j.contains("robbery") && j["robbery"].is_boolean()) crime.robbery = j["robbery"].get<bool>();
    if (j.contains("assault") && j["assault"].is_boolean()) crime.assault = j["assault"].get<bool>();
    if (j.contains("burglary") && j["burglary"].is_boolean()) crime.burglary = j["burglary"].get<bool>();
    if (j.contains("theft") && j["theft"].is_boolean()) crime.theft = j["theft"].get<bool>();
    if (j.contains("auto_theft") && j["auto_theft"].is_boolean()) crime.auto_theft = j["auto_theft"].get<bool>();
    if (j.contains("drug") && j["drug"].is_boolean()) crime.drug = j["drug"].get<bool>();
    if (j.contains("shooting") && j["shooting"].is_boolean()) crime.shooting = j["shooting"].get<bool>();
    if (j.contains("use_year") && j["use_year"].is_boolean()) crime.use_year = j["use_year"].get<bool>();
    if (j.contains("year_min") && j["year_min"].is_number_integer()) crime.year_min = j["year_min"].get<int>();
    if (j.contains("year_max") && j["year_max"].is_number_integer()) crime.year_max = j["year_max"].get<int>();
}

json queryRecordJson(const QueryRecord& q) {
    return {
        {"enabled", q.enabled},
        {"executed_at_utc", q.executed_at_utc},
        {"mode", q.mode},
        {"name", q.name},
        {"sql", q.sql},
        {"color", colorJson(q.color)},
        {"outline_color", colorJson(q.outline_color)},
        {"result_set", filterResultSetJson(q.result_set)},
        {"row_count", q.row_count},
        {"status", q.status},
        {"snapshot", {
            {"filter_enabled", q.snapshot.filter_enabled},
            {"filter_use_date", q.snapshot.filter_use_date},
            {"filter_year_min", q.snapshot.filter_year_min},
            {"filter_year_max", q.snapshot.filter_year_max},
            {"filter_blocklot", q.snapshot.filter_blocklot},
            {"filter_status", q.snapshot.filter_status},
            {"filter_address", q.snapshot.filter_address},
            {"filter_owner", q.snapshot.filter_owner},
            {"filter_zip", q.snapshot.filter_zip},
            {"crime", crimeFilterJson(q.snapshot.crime)},
            {"selected_owners", q.snapshot.selected_owners},
            {"selected_parcel_blocklots", q.snapshot.selected_parcel_blocklots},
            {"event_sector_enabled", stringBoolMapJson(q.snapshot.event_sector_enabled)},
            {"center_lon", q.snapshot.center_lon},
            {"center_lat", q.snapshot.center_lat},
            {"zoom", q.snapshot.zoom},
            {"map_title_text", q.snapshot.map_title_text},
            {"map_title_show_primary_parcel_source", q.snapshot.map_title_show_primary_parcel_source},
            {"map_title_source_layer_file", q.snapshot.map_title_source_layer_file}
        }}
    };
}

void loadQueryRecordJson(const json& j, QueryRecord& q) {
    if (!j.is_object()) return;
    if (j.contains("enabled") && j["enabled"].is_boolean()) q.enabled = j["enabled"].get<bool>();
    if (j.contains("executed_at_utc") && j["executed_at_utc"].is_string()) q.executed_at_utc = j["executed_at_utc"].get<std::string>();
    if (j.contains("mode") && j["mode"].is_string()) q.mode = j["mode"].get<std::string>();
    if (j.contains("name") && j["name"].is_string()) q.name = j["name"].get<std::string>();
    if (j.contains("sql") && j["sql"].is_string()) q.sql = j["sql"].get<std::string>();
    if (j.contains("color")) loadColorJson(j["color"], q.color);
    if (j.contains("outline_color")) loadColorJson(j["outline_color"], q.outline_color);
    else if (j.contains("color")) loadColorJson(j["color"], q.outline_color);
    if (j.contains("result_set")) loadFilterResultSetJson(j["result_set"], q.result_set);
    if (j.contains("row_count") && j["row_count"].is_number_unsigned()) q.row_count = j["row_count"].get<size_t>();
    if (j.contains("status") && j["status"].is_string()) q.status = j["status"].get<std::string>();
    if (!j.contains("snapshot") || !j["snapshot"].is_object()) return;
    const json& s = j["snapshot"];
    if (s.contains("filter_enabled") && s["filter_enabled"].is_boolean()) q.snapshot.filter_enabled = s["filter_enabled"].get<bool>();
    if (s.contains("filter_use_date") && s["filter_use_date"].is_boolean()) q.snapshot.filter_use_date = s["filter_use_date"].get<bool>();
    if (s.contains("filter_year_min") && s["filter_year_min"].is_number_integer()) q.snapshot.filter_year_min = s["filter_year_min"].get<int>();
    if (s.contains("filter_year_max") && s["filter_year_max"].is_number_integer()) q.snapshot.filter_year_max = s["filter_year_max"].get<int>();
    if (s.contains("filter_blocklot") && s["filter_blocklot"].is_string()) q.snapshot.filter_blocklot = s["filter_blocklot"].get<std::string>();
    if (s.contains("filter_status") && s["filter_status"].is_string()) q.snapshot.filter_status = s["filter_status"].get<std::string>();
    if (s.contains("filter_address") && s["filter_address"].is_string()) q.snapshot.filter_address = s["filter_address"].get<std::string>();
    if (s.contains("filter_owner") && s["filter_owner"].is_string()) q.snapshot.filter_owner = s["filter_owner"].get<std::string>();
    if (s.contains("filter_zip") && s["filter_zip"].is_string()) q.snapshot.filter_zip = s["filter_zip"].get<std::string>();
    if (s.contains("crime")) loadCrimeFilterJson(s["crime"], q.snapshot.crime);
    if (s.contains("selected_owners") && s["selected_owners"].is_array()) {
        q.snapshot.selected_owners.clear();
        for (const auto& item : s["selected_owners"]) if (item.is_string()) q.snapshot.selected_owners.push_back(item.get<std::string>());
    }
    if (s.contains("selected_parcel_blocklots") && s["selected_parcel_blocklots"].is_array()) {
        q.snapshot.selected_parcel_blocklots.clear();
        for (const auto& item : s["selected_parcel_blocklots"]) if (item.is_string()) q.snapshot.selected_parcel_blocklots.push_back(item.get<std::string>());
    }
    if (s.contains("event_sector_enabled")) loadStringBoolMapJson(s["event_sector_enabled"], q.snapshot.event_sector_enabled);
    if (s.contains("center_lon") && s["center_lon"].is_number()) q.snapshot.center_lon = s["center_lon"].get<double>();
    if (s.contains("center_lat") && s["center_lat"].is_number()) q.snapshot.center_lat = s["center_lat"].get<double>();
    if (s.contains("zoom") && s["zoom"].is_number()) q.snapshot.zoom = s["zoom"].get<double>();
    if (s.contains("map_title_text") && s["map_title_text"].is_string()) q.snapshot.map_title_text = s["map_title_text"].get<std::string>();
    if (s.contains("map_title_show_primary_parcel_source") && s["map_title_show_primary_parcel_source"].is_boolean()) {
        q.snapshot.map_title_show_primary_parcel_source = s["map_title_show_primary_parcel_source"].get<bool>();
    }
    if (s.contains("map_title_source_layer_file") && s["map_title_source_layer_file"].is_string()) {
        q.snapshot.map_title_source_layer_file = s["map_title_source_layer_file"].get<std::string>();
    }
}

template <typename T>
void addLayerVectorSetting(json& layer_json, const char* key, const std::vector<T>* values, size_t idx) {
    if (!values || idx >= values->size()) return;
    layer_json[key] = (*values)[idx];
}

int layerIndexForProjectFile(const std::vector<LayerDef>& layers, const std::string& file) {
    for (size_t i = 0; i < layers.size(); ++i) if (layers[i].file == file) return (int)i;
    return -1;
}

json buildProjectStatePayload(const RightPanelContext& ctx) {
    json layers = json::array();
    if (ctx.layers) {
        for (size_t i = 0; i < ctx.layers->size(); ++i) {
            const LayerDef& layer = (*ctx.layers)[i];
            json lj = {
                {"file", layer.file},
                {"name", layer.name},
                {"enabled", layer.enabled},
                {"color", colorJson(layer.color)},
                {"outline_color", colorJson(layer.outline_color)}
            };
            addLayerVectorSetting(lj, "fill_enabled", ctx.layer_fill_enabled, i);
            addLayerVectorSetting(lj, "hover_enabled", ctx.layer_hover_enabled, i);
            addLayerVectorSetting(lj, "inspect_enabled", ctx.layer_inspect_enabled, i);
            addLayerVectorSetting(lj, "heatmap_enabled", ctx.layer_heatmap_enabled, i);
            addLayerVectorSetting(lj, "heatmap_max_zoom", ctx.layer_heatmap_max_zoom, i);
            addLayerVectorSetting(lj, "parcel_detail_min_zoom", ctx.layer_parcel_detail_min_zoom, i);
            addLayerVectorSetting(lj, "heatmap_algo", ctx.layer_heatmap_algo, i);
            addLayerVectorSetting(lj, "normalize_mode", ctx.layer_normalize_mode, i);
            addLayerVectorSetting(lj, "heatmap_cell_px", ctx.layer_heatmap_cell_px, i);
            addLayerVectorSetting(lj, "heatmap_bandwidth_px", ctx.layer_heatmap_bandwidth_px, i);
            addLayerVectorSetting(lj, "heatmap_blur_sigma_px", ctx.layer_heatmap_blur_sigma_px, i);
            addLayerVectorSetting(lj, "heatmap_percentile_clip", ctx.layer_heatmap_percentile_clip, i);
            addLayerVectorSetting(lj, "choropleth_gamma", ctx.layer_choropleth_gamma, i);
            addLayerVectorSetting(lj, "heatmap_zoom_adaptive_bandwidth", ctx.layer_heatmap_zoom_adaptive_bandwidth, i);
            addLayerVectorSetting(lj, "heatmap_multires_enabled", ctx.layer_heatmap_multires_enabled, i);
            addLayerVectorSetting(lj, "heatmap_multires_blend", ctx.layer_heatmap_multires_blend, i);
            addLayerVectorSetting(lj, "heatmap_use_gradient", ctx.layer_heatmap_use_gradient, i);
            layers.push_back(std::move(lj));
        }
    }

    json query_layers = json::array();
    if (ctx.query_layers) for (const QueryRecord& q : *ctx.query_layers) query_layers.push_back(queryRecordJson(q));
    json query_history = json::array();
    if (ctx.query_history) for (const QueryRecord& q : *ctx.query_history) query_history.push_back(queryRecordJson(q));

    json parcel_refs = json::array();
    if (ctx.parcel_selection) {
        for (const ParcelSelectionRef& ref : ctx.parcel_selection->refs) {
            parcel_refs.push_back({
                {"layer_idx", ref.layer_idx},
                {"geometry_entity_id", ref.geometry_entity_id},
                {"entity_id", ref.entity_id},
                {"feature_idx", ref.feature_idx}
            });
        }
    }

    json road_labels = json::object();
    road_labels["visible"] = ctx.road_label_state ? ctx.road_label_state->visible : true;
    road_labels["selected_idx"] = ctx.road_label_state ? ctx.road_label_state->selected_idx : (size_t)-1;
    road_labels["selections"] = json::array();
    if (ctx.road_label_state) {
        for (const RoadLabelSelection& label : ctx.road_label_state->selections) {
            road_labels["selections"].push_back({
                {"label", label.label},
                {"layer_idx", label.layer_idx},
                {"feature_idx", label.feature_idx},
                {"anchor_lon", label.anchor_lonlat.x},
                {"anchor_lat", label.anchor_lonlat.y},
                {"angle_rad", label.angle_rad}
            });
        }
    }

    json app = json::object();
    if (ctx.app_settings) {
        app = {
            {"dark_mode", ctx.app_settings->dark_mode},
            {"grayscale_basemap", ctx.app_settings->grayscale_basemap},
            {"basemap_osm_enabled", ctx.app_settings->basemap_osm_enabled},
            {"basemap_topographic_enabled", ctx.app_settings->basemap_topographic_enabled},
            {"basemap_satellite_enabled", ctx.app_settings->basemap_satellite_enabled},
            {"basemap_dark_satellite_enabled", ctx.app_settings->basemap_dark_satellite_enabled},
            {"basemap_night_satellite_enabled", ctx.app_settings->basemap_night_satellite_enabled},
            {"basemap_osm_opacity", ctx.app_settings->basemap_osm_opacity},
            {"basemap_topographic_opacity", ctx.app_settings->basemap_topographic_opacity},
            {"basemap_satellite_opacity", ctx.app_settings->basemap_satellite_opacity},
            {"basemap_dark_satellite_opacity", ctx.app_settings->basemap_dark_satellite_opacity},
            {"basemap_night_satellite_opacity", ctx.app_settings->basemap_night_satellite_opacity},
            {"map_polygon_fill_opacity", ctx.app_settings->map_polygon_fill_opacity},
            {"map_polygon_outline_thickness", ctx.app_settings->map_polygon_outline_thickness},
            {"zoom_step", ctx.app_settings->zoom_step},
            {"smooth_scroll_zoom", ctx.app_settings->smooth_scroll_zoom},
            {"scroll_zoom_distance", ctx.app_settings->scroll_zoom_distance},
            {"scroll_zoom_duration_s", ctx.app_settings->scroll_zoom_duration_s},
            {"alt_zoom_number_multiplier", ctx.app_settings->alt_zoom_number_multiplier},
            {"road_label_click_mode", ctx.app_settings->road_label_click_mode},
            {"road_label_pick_tolerance_px", ctx.app_settings->road_label_pick_tolerance_px},
            {"road_label_size_scale", ctx.app_settings->road_label_size_scale},
            {"road_label_angle_along_road", ctx.app_settings->road_label_angle_along_road},
            {"road_label_avoid_overlap", ctx.app_settings->road_label_avoid_overlap},
            {"road_label_collision_mode", ctx.app_settings->road_label_collision_mode},
            {"road_label_separation_px", ctx.app_settings->road_label_separation_px},
            {"topo_vector_enabled", ctx.app_settings->topo_vector_enabled},
            {"zoning_use_simcity_colors", ctx.app_settings->zoning_use_simcity_colors},
            {"reserve_cpu_cores", ctx.app_settings->reserve_cpu_cores},
            {"map_title_text", ctx.app_settings->map_title_text},
            {"map_title_show_primary_parcel_source", ctx.app_settings->map_title_show_primary_parcel_source},
            {"map_title_source_layer_file", ctx.app_settings->map_title_source_layer_file},
            {"map_title_all_caps", ctx.app_settings->map_title_all_caps},
            {"map_legend_show_overlay", ctx.app_settings->map_legend_show_overlay},
            {"map_legend_overlay_position", ctx.app_settings->map_legend_overlay_position}
        };
    }

    const MapFilterState* f = ctx.map_filter_state;
    json filters = f ? json{
        {"enabled", f->enabled},
        {"use_date", f->use_date},
        {"year_min", f->year_min},
        {"year_max", f->year_max},
        {"blocklot", f->blocklot},
        {"status", f->status},
        {"address", f->address},
        {"owner", f->owner},
        {"zip", f->zip},
        {"crime", crimeFilterJson(f->crime)},
        {"selected_owners", stringSetJson(f->selected_owners)},
        {"event_sector_enabled", stringBoolMapJson(f->event_sector_enabled)}
    } : json::object();

    return {
        {"app_settings", std::move(app)},
        {"map", {
            {"center_lon", ctx.center_lon ? *ctx.center_lon : 0.0},
            {"center_lat", ctx.center_lat ? *ctx.center_lat : 0.0},
            {"zoom", ctx.zoom ? *ctx.zoom : 0.0}
        }},
        {"layers", std::move(layers)},
        {"active_hover_layer_idx", ctx.active_hover_layer_idx ? *ctx.active_hover_layer_idx : -1},
        {"active_click_layer_idx", ctx.active_click_layer_idx ? *ctx.active_click_layer_idx : -1},
        {"parcel_parameter_mode", ctx.parcel_parameter_mode_ptr ? *ctx.parcel_parameter_mode_ptr : ctx.parcel_parameter_mode},
        {"global_heatmap", {
            {"heatmap_algo", ctx.heatmap_algo_ptr ? *ctx.heatmap_algo_ptr : ctx.heatmap_algo},
            {"heatmap_quality_preset", ctx.heatmap_quality_preset ? *ctx.heatmap_quality_preset : 0},
            {"global_heat_cell_px", ctx.global_heat_cell_px ? *ctx.global_heat_cell_px : 24.0f},
            {"heatmap_bandwidth_px", ctx.heatmap_bandwidth_px ? *ctx.heatmap_bandwidth_px : 18.0f},
            {"heatmap_blur_sigma_px", ctx.heatmap_blur_sigma_px ? *ctx.heatmap_blur_sigma_px : 6.0f},
            {"heatmap_percentile_clip", ctx.global_heatmap_percentile_clip ? *ctx.global_heatmap_percentile_clip : ctx.heatmap_percentile_clip},
            {"heatmap_zoom_adaptive_bandwidth", ctx.heatmap_zoom_adaptive_bandwidth ? *ctx.heatmap_zoom_adaptive_bandwidth : true},
            {"heatmap_multires_enabled", ctx.heatmap_multires_enabled ? *ctx.heatmap_multires_enabled : true},
            {"heatmap_multires_blend", ctx.heatmap_multires_blend ? *ctx.heatmap_multires_blend : 0.5f}
        }},
        {"filters", std::move(filters)},
        {"parcel_jurisdictions", ctx.parcel_jurisdiction_filter_state ? stringSetJson(ctx.parcel_jurisdiction_filter_state->selected_jurisdictions) : json::array()},
        {"road_labels", std::move(road_labels)},
        {"selection", {
            {"parcel_show_details", ctx.parcel_selection ? ctx.parcel_selection->show_details : false},
            {"parcel_group_highlight", ctx.parcel_selection ? ctx.parcel_selection->group_highlight : false},
            {"parcel_active_layer_idx", ctx.parcel_selection ? ctx.parcel_selection->active_layer_idx : -1},
            {"parcel_active_entity_id", ctx.parcel_selection ? ctx.parcel_selection->active_entity_id : std::string()},
            {"parcel_refs", std::move(parcel_refs)},
            {"show_selected_zone_details", ctx.show_selected_zone_details ? *ctx.show_selected_zone_details : false},
            {"selected_zone_idx", ctx.selected_zone_idx ? *ctx.selected_zone_idx : (size_t)-1}
        }},
        {"query_layers", std::move(query_layers)},
        {"query_history", std::move(query_history)}
    };
}

template <typename T>
void loadLayerVectorSetting(const json& layer_json, const char* key, std::vector<T>* values, size_t idx) {
    if (!values || !layer_json.contains(key) || idx >= values->size()) return;
    const json& v = layer_json[key];
    if constexpr (std::is_same_v<T, bool>) {
        if (v.is_boolean()) (*values)[idx] = v.get<bool>();
    } else if constexpr (std::is_integral_v<T>) {
        if (v.is_number_integer()) (*values)[idx] = v.get<T>();
    } else {
        if (v.is_number()) (*values)[idx] = v.get<T>();
    }
}

bool applyProjectJson(const RightPanelContext& ctx, const json& project, std::string& message) {
    if (!project.is_object()) {
        message = "Project file is not a JSON object.";
        return false;
    }
    if (ctx.app_settings && project.contains("app_settings") && project["app_settings"].is_object()) {
        const json& a = project["app_settings"];
        if (a.contains("dark_mode") && a["dark_mode"].is_boolean()) ctx.app_settings->dark_mode = a["dark_mode"].get<bool>();
        if (a.contains("grayscale_basemap") && a["grayscale_basemap"].is_boolean()) ctx.app_settings->grayscale_basemap = a["grayscale_basemap"].get<bool>();
        if (a.contains("basemap_osm_enabled") && a["basemap_osm_enabled"].is_boolean()) ctx.app_settings->basemap_osm_enabled = a["basemap_osm_enabled"].get<bool>();
        if (a.contains("basemap_topographic_enabled") && a["basemap_topographic_enabled"].is_boolean()) ctx.app_settings->basemap_topographic_enabled = a["basemap_topographic_enabled"].get<bool>();
        if (a.contains("basemap_satellite_enabled") && a["basemap_satellite_enabled"].is_boolean()) ctx.app_settings->basemap_satellite_enabled = a["basemap_satellite_enabled"].get<bool>();
        if (a.contains("basemap_dark_satellite_enabled") && a["basemap_dark_satellite_enabled"].is_boolean()) ctx.app_settings->basemap_dark_satellite_enabled = a["basemap_dark_satellite_enabled"].get<bool>();
        if (a.contains("basemap_night_satellite_enabled") && a["basemap_night_satellite_enabled"].is_boolean()) ctx.app_settings->basemap_night_satellite_enabled = a["basemap_night_satellite_enabled"].get<bool>();
        if (a.contains("basemap_osm_opacity") && a["basemap_osm_opacity"].is_number()) ctx.app_settings->basemap_osm_opacity = std::clamp(a["basemap_osm_opacity"].get<float>(), 0.0f, 1.0f);
        if (a.contains("basemap_topographic_opacity") && a["basemap_topographic_opacity"].is_number()) ctx.app_settings->basemap_topographic_opacity = std::clamp(a["basemap_topographic_opacity"].get<float>(), 0.0f, 1.0f);
        if (a.contains("basemap_satellite_opacity") && a["basemap_satellite_opacity"].is_number()) ctx.app_settings->basemap_satellite_opacity = std::clamp(a["basemap_satellite_opacity"].get<float>(), 0.0f, 1.0f);
        if (a.contains("basemap_dark_satellite_opacity") && a["basemap_dark_satellite_opacity"].is_number()) ctx.app_settings->basemap_dark_satellite_opacity = std::clamp(a["basemap_dark_satellite_opacity"].get<float>(), 0.0f, 1.0f);
        if (a.contains("basemap_night_satellite_opacity") && a["basemap_night_satellite_opacity"].is_number()) ctx.app_settings->basemap_night_satellite_opacity = std::clamp(a["basemap_night_satellite_opacity"].get<float>(), 0.0f, 1.0f);
        if (a.contains("map_polygon_fill_opacity") && a["map_polygon_fill_opacity"].is_number()) ctx.app_settings->map_polygon_fill_opacity = std::clamp(a["map_polygon_fill_opacity"].get<float>(), 0.0f, 1.0f);
        if (a.contains("map_polygon_outline_thickness") && a["map_polygon_outline_thickness"].is_number()) ctx.app_settings->map_polygon_outline_thickness = std::clamp(a["map_polygon_outline_thickness"].get<float>(), 1.0f, 8.0f);
        if (a.contains("zoom_step") && a["zoom_step"].is_number()) ctx.app_settings->zoom_step = std::clamp(a["zoom_step"].get<double>(), 0.05, 4.0);
        if (a.contains("smooth_scroll_zoom") && a["smooth_scroll_zoom"].is_boolean()) ctx.app_settings->smooth_scroll_zoom = a["smooth_scroll_zoom"].get<bool>();
        if (a.contains("scroll_zoom_distance") && a["scroll_zoom_distance"].is_number()) ctx.app_settings->scroll_zoom_distance = std::clamp(a["scroll_zoom_distance"].get<double>(), 0.01, 4.0);
        if (a.contains("scroll_zoom_duration_s") && a["scroll_zoom_duration_s"].is_number()) ctx.app_settings->scroll_zoom_duration_s = std::clamp(a["scroll_zoom_duration_s"].get<double>(), 0.0, 1.5);
        if (a.contains("alt_zoom_number_multiplier") && a["alt_zoom_number_multiplier"].is_number()) ctx.app_settings->alt_zoom_number_multiplier = std::clamp(a["alt_zoom_number_multiplier"].get<double>(), 0.25, 16.0);
        if (a.contains("road_label_click_mode") && a["road_label_click_mode"].is_boolean()) ctx.app_settings->road_label_click_mode = a["road_label_click_mode"].get<bool>();
        if (a.contains("road_label_pick_tolerance_px") && a["road_label_pick_tolerance_px"].is_number()) ctx.app_settings->road_label_pick_tolerance_px = std::clamp(a["road_label_pick_tolerance_px"].get<float>(), 4.0f, 32.0f);
        if (a.contains("road_label_size_scale") && a["road_label_size_scale"].is_number()) ctx.app_settings->road_label_size_scale = std::clamp(a["road_label_size_scale"].get<float>(), 0.65f, 2.5f);
        if (a.contains("road_label_angle_along_road") && a["road_label_angle_along_road"].is_boolean()) ctx.app_settings->road_label_angle_along_road = a["road_label_angle_along_road"].get<bool>();
        if (a.contains("road_label_avoid_overlap") && a["road_label_avoid_overlap"].is_boolean()) ctx.app_settings->road_label_avoid_overlap = a["road_label_avoid_overlap"].get<bool>();
        if (a.contains("road_label_collision_mode") && a["road_label_collision_mode"].is_number_integer()) {
            ctx.app_settings->road_label_collision_mode = std::clamp(a["road_label_collision_mode"].get<int>(), 0, 3);
            ctx.app_settings->road_label_avoid_overlap = ctx.app_settings->road_label_collision_mode != 0;
        }
        if (a.contains("road_label_separation_px") && a["road_label_separation_px"].is_number()) ctx.app_settings->road_label_separation_px = std::clamp(a["road_label_separation_px"].get<float>(), 0.0f, 48.0f);
        if (a.contains("topo_vector_enabled") && a["topo_vector_enabled"].is_boolean()) ctx.app_settings->topo_vector_enabled = a["topo_vector_enabled"].get<bool>();
        if (a.contains("zoning_use_simcity_colors") && a["zoning_use_simcity_colors"].is_boolean()) ctx.app_settings->zoning_use_simcity_colors = a["zoning_use_simcity_colors"].get<bool>();
        if (a.contains("reserve_cpu_cores") && a["reserve_cpu_cores"].is_number_integer()) ctx.app_settings->reserve_cpu_cores = std::max(0, a["reserve_cpu_cores"].get<int>());
        if (a.contains("map_title_text") && a["map_title_text"].is_string()) ctx.app_settings->map_title_text = a["map_title_text"].get<std::string>();
        if (a.contains("map_title_show_primary_parcel_source") && a["map_title_show_primary_parcel_source"].is_boolean()) ctx.app_settings->map_title_show_primary_parcel_source = a["map_title_show_primary_parcel_source"].get<bool>();
        if (a.contains("map_title_source_layer_file") && a["map_title_source_layer_file"].is_string()) ctx.app_settings->map_title_source_layer_file = a["map_title_source_layer_file"].get<std::string>();
        if (a.contains("map_title_all_caps") && a["map_title_all_caps"].is_boolean()) ctx.app_settings->map_title_all_caps = a["map_title_all_caps"].get<bool>();
        if (a.contains("map_legend_show_overlay") && a["map_legend_show_overlay"].is_boolean()) ctx.app_settings->map_legend_show_overlay = a["map_legend_show_overlay"].get<bool>();
        if (a.contains("map_legend_overlay_position") && a["map_legend_overlay_position"].is_number_integer()) ctx.app_settings->map_legend_overlay_position = std::clamp(a["map_legend_overlay_position"].get<int>(), 0, 3);
        if (ctx.root) saveAppSettings(*ctx.root, *ctx.app_settings);
    }
    if (project.contains("map") && project["map"].is_object()) {
        const json& m = project["map"];
        if (ctx.center_lon && m.contains("center_lon") && m["center_lon"].is_number()) *ctx.center_lon = m["center_lon"].get<double>();
        if (ctx.center_lat && m.contains("center_lat") && m["center_lat"].is_number()) *ctx.center_lat = std::clamp(m["center_lat"].get<double>(), -85.0, 85.0);
        if (ctx.zoom && m.contains("zoom") && m["zoom"].is_number()) *ctx.zoom = std::clamp(m["zoom"].get<double>(), (double)ctx.min_zoom, (double)ctx.max_zoom);
    }
    if (ctx.layers && project.contains("layers") && project["layers"].is_array()) {
        for (const auto& item : project["layers"]) {
            if (!item.is_object() || !item.contains("file") || !item["file"].is_string()) continue;
            const int idx = layerIndexForProjectFile(*ctx.layers, item["file"].get<std::string>());
            if (idx < 0) continue;
            LayerDef& layer = (*ctx.layers)[(size_t)idx];
            if (item.contains("enabled") && item["enabled"].is_boolean()) layer.enabled = item["enabled"].get<bool>();
            if (item.contains("color")) loadColorJson(item["color"], layer.color);
            if (item.contains("outline_color")) loadColorJson(item["outline_color"], layer.outline_color);
            loadLayerVectorSetting(item, "fill_enabled", ctx.layer_fill_enabled, (size_t)idx);
            loadLayerVectorSetting(item, "hover_enabled", ctx.layer_hover_enabled, (size_t)idx);
            loadLayerVectorSetting(item, "inspect_enabled", ctx.layer_inspect_enabled, (size_t)idx);
            loadLayerVectorSetting(item, "heatmap_enabled", ctx.layer_heatmap_enabled, (size_t)idx);
            loadLayerVectorSetting(item, "heatmap_max_zoom", ctx.layer_heatmap_max_zoom, (size_t)idx);
            loadLayerVectorSetting(item, "parcel_detail_min_zoom", ctx.layer_parcel_detail_min_zoom, (size_t)idx);
            loadLayerVectorSetting(item, "heatmap_algo", ctx.layer_heatmap_algo, (size_t)idx);
            loadLayerVectorSetting(item, "normalize_mode", ctx.layer_normalize_mode, (size_t)idx);
            loadLayerVectorSetting(item, "heatmap_cell_px", ctx.layer_heatmap_cell_px, (size_t)idx);
            loadLayerVectorSetting(item, "heatmap_bandwidth_px", ctx.layer_heatmap_bandwidth_px, (size_t)idx);
            loadLayerVectorSetting(item, "heatmap_blur_sigma_px", ctx.layer_heatmap_blur_sigma_px, (size_t)idx);
            loadLayerVectorSetting(item, "heatmap_percentile_clip", ctx.layer_heatmap_percentile_clip, (size_t)idx);
            loadLayerVectorSetting(item, "choropleth_gamma", ctx.layer_choropleth_gamma, (size_t)idx);
            loadLayerVectorSetting(item, "heatmap_zoom_adaptive_bandwidth", ctx.layer_heatmap_zoom_adaptive_bandwidth, (size_t)idx);
            loadLayerVectorSetting(item, "heatmap_multires_enabled", ctx.layer_heatmap_multires_enabled, (size_t)idx);
            loadLayerVectorSetting(item, "heatmap_multires_blend", ctx.layer_heatmap_multires_blend, (size_t)idx);
            loadLayerVectorSetting(item, "heatmap_use_gradient", ctx.layer_heatmap_use_gradient, (size_t)idx);
        }
        if (ctx.layer_fill_state_changed) *ctx.layer_fill_state_changed = true;
        if (ctx.layer_hover_state_changed) *ctx.layer_hover_state_changed = true;
        if (ctx.layer_inspect_state_changed) *ctx.layer_inspect_state_changed = true;
        if (ctx.layer_heatmap_state_changed) *ctx.layer_heatmap_state_changed = true;
    }
    if (ctx.active_hover_layer_idx && project.contains("active_hover_layer_idx") && project["active_hover_layer_idx"].is_number_integer()) *ctx.active_hover_layer_idx = project["active_hover_layer_idx"].get<int>();
    if (ctx.active_click_layer_idx && project.contains("active_click_layer_idx") && project["active_click_layer_idx"].is_number_integer()) *ctx.active_click_layer_idx = project["active_click_layer_idx"].get<int>();
    if (ctx.parcel_parameter_mode_ptr && project.contains("parcel_parameter_mode") && project["parcel_parameter_mode"].is_number_integer()) *ctx.parcel_parameter_mode_ptr = project["parcel_parameter_mode"].get<int>();
    if (project.contains("global_heatmap") && project["global_heatmap"].is_object()) {
        const json& h = project["global_heatmap"];
        if (ctx.heatmap_algo_ptr && h.contains("heatmap_algo") && h["heatmap_algo"].is_number_integer()) *ctx.heatmap_algo_ptr = h["heatmap_algo"].get<int>();
        if (ctx.heatmap_quality_preset && h.contains("heatmap_quality_preset") && h["heatmap_quality_preset"].is_number_integer()) *ctx.heatmap_quality_preset = h["heatmap_quality_preset"].get<int>();
        if (ctx.global_heat_cell_px && h.contains("global_heat_cell_px") && h["global_heat_cell_px"].is_number()) *ctx.global_heat_cell_px = h["global_heat_cell_px"].get<float>();
        if (ctx.heatmap_bandwidth_px && h.contains("heatmap_bandwidth_px") && h["heatmap_bandwidth_px"].is_number()) *ctx.heatmap_bandwidth_px = h["heatmap_bandwidth_px"].get<float>();
        if (ctx.heatmap_blur_sigma_px && h.contains("heatmap_blur_sigma_px") && h["heatmap_blur_sigma_px"].is_number()) *ctx.heatmap_blur_sigma_px = h["heatmap_blur_sigma_px"].get<float>();
        if (ctx.global_heatmap_percentile_clip && h.contains("heatmap_percentile_clip") && h["heatmap_percentile_clip"].is_number()) *ctx.global_heatmap_percentile_clip = h["heatmap_percentile_clip"].get<float>();
        if (ctx.heatmap_zoom_adaptive_bandwidth && h.contains("heatmap_zoom_adaptive_bandwidth") && h["heatmap_zoom_adaptive_bandwidth"].is_boolean()) *ctx.heatmap_zoom_adaptive_bandwidth = h["heatmap_zoom_adaptive_bandwidth"].get<bool>();
        if (ctx.heatmap_multires_enabled && h.contains("heatmap_multires_enabled") && h["heatmap_multires_enabled"].is_boolean()) *ctx.heatmap_multires_enabled = h["heatmap_multires_enabled"].get<bool>();
        if (ctx.heatmap_multires_blend && h.contains("heatmap_multires_blend") && h["heatmap_multires_blend"].is_number()) *ctx.heatmap_multires_blend = h["heatmap_multires_blend"].get<float>();
        if (ctx.layer_heatmap_state_changed) *ctx.layer_heatmap_state_changed = true;
    }
    if (ctx.map_filter_state && project.contains("filters") && project["filters"].is_object()) {
        const json& f = project["filters"];
        if (f.contains("enabled") && f["enabled"].is_boolean()) ctx.map_filter_state->enabled = f["enabled"].get<bool>();
        if (f.contains("use_date") && f["use_date"].is_boolean()) ctx.map_filter_state->use_date = f["use_date"].get<bool>();
        if (f.contains("year_min") && f["year_min"].is_number_integer()) ctx.map_filter_state->year_min = f["year_min"].get<int>();
        if (f.contains("year_max") && f["year_max"].is_number_integer()) ctx.map_filter_state->year_max = f["year_max"].get<int>();
        copyProjectText(f, "blocklot", ctx.map_filter_state->blocklot, sizeof(ctx.map_filter_state->blocklot));
        copyProjectText(f, "status", ctx.map_filter_state->status, sizeof(ctx.map_filter_state->status));
        copyProjectText(f, "address", ctx.map_filter_state->address, sizeof(ctx.map_filter_state->address));
        copyProjectText(f, "owner", ctx.map_filter_state->owner, sizeof(ctx.map_filter_state->owner));
        copyProjectText(f, "zip", ctx.map_filter_state->zip, sizeof(ctx.map_filter_state->zip));
        if (f.contains("crime")) loadCrimeFilterJson(f["crime"], ctx.map_filter_state->crime);
        if (f.contains("selected_owners")) loadStringSetJson(f["selected_owners"], ctx.map_filter_state->selected_owners);
        if (f.contains("event_sector_enabled")) loadStringBoolMapJson(f["event_sector_enabled"], ctx.map_filter_state->event_sector_enabled);
    }
    if (ctx.parcel_jurisdiction_filter_state && project.contains("parcel_jurisdictions")) {
        loadStringSetJson(project["parcel_jurisdictions"], ctx.parcel_jurisdiction_filter_state->selected_jurisdictions);
        ctx.parcel_jurisdiction_filter_state->dirty = true;
    }
    if (ctx.road_label_state && project.contains("road_labels") && project["road_labels"].is_object()) {
        const json& r = project["road_labels"];
        if (r.contains("visible") && r["visible"].is_boolean()) ctx.road_label_state->visible = r["visible"].get<bool>();
        ctx.road_label_state->selected_idx =
            r.contains("selected_idx") && r["selected_idx"].is_number_unsigned()
                ? r["selected_idx"].get<size_t>()
                : (size_t)-1;
        ctx.road_label_state->selections.clear();
        if (r.contains("selections") && r["selections"].is_array()) {
            for (const auto& item : r["selections"]) {
                if (!item.is_object()) continue;
                RoadLabelSelection label;
                label.label = item.value("label", std::string());
                label.layer_idx = item.value("layer_idx", -1);
                label.feature_idx =
                    item.contains("feature_idx") && item["feature_idx"].is_number_unsigned()
                        ? item["feature_idx"].get<size_t>()
                        : (size_t)-1;
                if (item.contains("anchor_lon") && item["anchor_lon"].is_number()) label.anchor_lonlat.x = item["anchor_lon"].get<float>();
                if (item.contains("anchor_lat") && item["anchor_lat"].is_number()) label.anchor_lonlat.y = item["anchor_lat"].get<float>();
                if (item.contains("angle_rad") && item["angle_rad"].is_number()) label.angle_rad = item["angle_rad"].get<float>();
                if (!label.label.empty()) ctx.road_label_state->selections.push_back(std::move(label));
            }
        }
        if (ctx.road_label_state->selected_idx >= ctx.road_label_state->selections.size()) {
            ctx.road_label_state->selected_idx = (size_t)-1;
        }
    }
    if (project.contains("selection") && project["selection"].is_object()) {
        const json& s = project["selection"];
        if (ctx.parcel_selection) {
            clearParcelSelection(*ctx.parcel_selection);
            if (s.contains("parcel_refs") && s["parcel_refs"].is_array()) {
                for (const auto& item : s["parcel_refs"]) {
                    if (!item.is_object()) continue;
                    const int layer_idx = item.value("layer_idx", -1);
                    const std::string entity_id = item.value("entity_id", std::string());
                    const std::string geometry_entity_id = item.value("geometry_entity_id", entity_id);
                    const size_t feature_idx = item.contains("feature_idx") && item["feature_idx"].is_number_unsigned()
                        ? item["feature_idx"].get<size_t>()
                        : (size_t)-1;
                    selectParcel(*ctx.parcel_selection, layer_idx, feature_idx, geometry_entity_id, entity_id, true);
                }
            }
            if (s.contains("parcel_group_highlight") && s["parcel_group_highlight"].is_boolean()) ctx.parcel_selection->group_highlight = s["parcel_group_highlight"].get<bool>();
        }
        if (ctx.show_selected_zone_details && s.contains("show_selected_zone_details") && s["show_selected_zone_details"].is_boolean()) *ctx.show_selected_zone_details = s["show_selected_zone_details"].get<bool>();
        if (ctx.selected_zone_idx && s.contains("selected_zone_idx") && s["selected_zone_idx"].is_number_unsigned()) *ctx.selected_zone_idx = s["selected_zone_idx"].get<size_t>();
    }
    if (ctx.query_layers && project.contains("query_layers") && project["query_layers"].is_array()) {
        ctx.query_layers->clear();
        for (const auto& item : project["query_layers"]) {
            QueryRecord q;
            loadQueryRecordJson(item, q);
            if (!q.sql.empty() || !q.name.empty()) ctx.query_layers->push_back(std::move(q));
        }
    }
    if (ctx.query_history && project.contains("query_history") && project["query_history"].is_array()) {
        ctx.query_history->clear();
        for (const auto& item : project["query_history"]) {
            QueryRecord q;
            loadQueryRecordJson(item, q);
            if (!q.sql.empty() || !q.name.empty()) ctx.query_history->push_back(std::move(q));
        }
    }
    message = "Loaded project.";
    return true;
}

bool saveProjectFile(const RightPanelContext& ctx, const std::filesystem::path& path, std::string& message) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        message = "Could not create project directory: " + ec.message();
        return false;
    }
    std::ofstream out(path);
    if (!out) {
        message = "Could not open project for writing.";
        return false;
    }
    out << buildWorldsimProjectDocument(buildProjectStatePayload(ctx)).dump(2);
    if (!out) {
        message = "Could not write project file.";
        return false;
    }
    if (ctx.current_project_path) *ctx.current_project_path = path.string();
    if (ctx.app_settings && ctx.root) {
        ctx.app_settings->last_project_path = path.string();
        saveAppSettings(*ctx.root, *ctx.app_settings);
    }
    message = "Saved project: " + path.filename().string();
    return true;
}

bool writeProjectDocumentAtomic(const std::filesystem::path& path, const std::string& document, std::string& message) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        message = "Could not create project directory: " + ec.message();
        return false;
    }
    const std::filesystem::path tmp_path = path.string() + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out) {
            message = "Could not open project temp file for writing.";
            return false;
        }
        out << document;
        if (!out) {
            message = "Could not write project temp file.";
            return false;
        }
    }
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        std::filesystem::remove(tmp_path);
        message = "Could not replace project file: " + ec.message();
        return false;
    }
    return true;
}

void autosaveCurrentProjectIfChanged(const RightPanelContext& ctx) {
    if (!ctx.current_project_path || ctx.current_project_path->empty()) return;
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last_attempt = Clock::time_point::min();
    static std::string last_path;
    static std::string last_document;

    const Clock::time_point now = Clock::now();
    if (last_attempt != Clock::time_point::min() &&
        now - last_attempt < std::chrono::milliseconds(200)) {
        return;
    }
    last_attempt = now;

    const std::filesystem::path path(*ctx.current_project_path);
    const std::string current_path = path.string();
    if (current_path != last_path) {
        last_path = current_path;
        last_document.clear();
        std::ifstream in(path);
        if (in) {
            last_document.assign(
                std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
        }
    }

    const std::string document =
        buildWorldsimProjectDocument(buildProjectStatePayload(ctx)).dump(2);
    if (document == last_document) return;

    std::string message;
    if (writeProjectDocumentAtomic(path, document, message)) {
        last_document = document;
        if (ctx.app_settings && ctx.root && ctx.app_settings->last_project_path != current_path) {
            ctx.app_settings->last_project_path = current_path;
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
    } else if (ctx.project_status) {
        *ctx.project_status = "Autosave failed: " + message;
    }
}

bool loadProjectFile(const RightPanelContext& ctx, const std::filesystem::path& path, std::string& message) {
    std::ifstream in(path);
    if (!in) {
        message = "Could not open project.";
        return false;
    }
    json project;
    try {
        in >> project;
    } catch (const std::exception& e) {
        message = std::string("Invalid project JSON: ") + e.what();
        return false;
    }
    const ProjectStateValidation validation = validateWorldsimProjectDocument(project);
    if (!validation.ok) {
        message = summarizeProjectValidation(validation);
        return false;
    }
    const json* state = worldsimProjectStatePayload(project);
    if (!state || !applyProjectJson(ctx, *state, message)) return false;
    if (ctx.current_project_path) *ctx.current_project_path = path.string();
    if (ctx.app_settings && ctx.root) {
        ctx.app_settings->last_project_path = path.string();
        saveAppSettings(*ctx.root, *ctx.app_settings);
    }
    message = "Loaded project: " + path.filename().string();
    const std::string validation_note = summarizeProjectValidation(validation);
    if (!validation_note.empty()) message += " (" + validation_note + ")";
    return true;
}

bool validateProjectFile(const std::filesystem::path& path, std::string& message) {
    std::ifstream in(path);
    if (!in) {
        message = "Could not open project.";
        return false;
    }
    json project;
    try {
        in >> project;
    } catch (const std::exception& e) {
        message = std::string("Invalid project JSON: ") + e.what();
        return false;
    }
    const ProjectStateValidation validation = validateWorldsimProjectDocument(project);
    if (!validation.ok) {
        message = summarizeProjectValidation(validation);
        return false;
    }
    message = "Valid project schema v" + std::to_string(validation.schema_version) + ".";
    if (!validation.warnings.empty()) message += " " + validation.warnings.front();
    return true;
}

std::vector<std::filesystem::path> listProjectFiles(const RightPanelContext& ctx) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    const std::filesystem::path dir = worldsimProjectsDir(ctx.root);
    if (!std::filesystem::exists(dir, ec) || ec) return out;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        if (it->path().extension() == ".json") out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void reloadLastProjectAtStartup(const RightPanelContext& ctx) {
    static bool attempted = false;
    if (attempted) return;
    attempted = true;
    if (!ctx.app_settings || !ctx.root || ctx.app_settings->last_project_path.empty()) return;
    if (ctx.current_project_path && !ctx.current_project_path->empty()) return;

    std::string message;
    const std::filesystem::path path = normalizeWorldsimProjectPath(ctx.root, ctx.app_settings->last_project_path);
    if (loadProjectFile(ctx, path, message)) {
        if (ctx.project_status) *ctx.project_status = "Reloaded startup project: " + path.filename().string();
    } else if (ctx.project_status) {
        *ctx.project_status = "Could not reload startup project: " + message;
    }
}

void drawProjectsTab(const RightPanelContext& ctx) {
    if (!ImGui::BeginTabItem("Projects")) return;
    static char project_path_buffer[512] = "project.json";
    static int selected_project_idx = -1;
    if (ctx.current_project_path && !ctx.current_project_path->empty()) {
        ImGui::TextWrapped("Current: %s", ctx.current_project_path->c_str());
    } else {
        ImGui::TextDisabled("No project file is active.");
    }
    ImGui::TextDisabled("Project schema: %s v%d", kWorldsimProjectSchemaId, kWorldsimProjectSchemaVersion);
    ImGui::InputText("Project file", project_path_buffer, sizeof(project_path_buffer));
    if (ImGui::Button("Save")) {
        std::string message;
        const std::string current = ctx.current_project_path ? *ctx.current_project_path : std::string();
        const std::filesystem::path path = current.empty()
            ? normalizeWorldsimProjectPath(ctx.root, project_path_buffer)
            : std::filesystem::path(current);
        saveProjectFile(ctx, path, message);
        if (ctx.project_status) *ctx.project_status = message;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save As")) {
        std::string message;
        saveProjectFile(ctx, normalizeWorldsimProjectPath(ctx.root, project_path_buffer), message);
        if (ctx.project_status) *ctx.project_status = message;
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Path")) {
        std::string message;
        loadProjectFile(ctx, normalizeWorldsimProjectPath(ctx.root, project_path_buffer), message);
        if (ctx.project_status) *ctx.project_status = message;
    }
    ImGui::SameLine();
    if (ImGui::Button("Validate")) {
        std::string message;
        validateProjectFile(normalizeWorldsimProjectPath(ctx.root, project_path_buffer), message);
        if (ctx.project_status) *ctx.project_status = message;
    }
    ImGui::SeparatorText("Existing Projects");
    const std::vector<std::filesystem::path> projects = listProjectFiles(ctx);
    if (projects.empty()) {
        ImGui::TextDisabled("No project files in data/projects.");
    } else {
        if (selected_project_idx >= (int)projects.size()) selected_project_idx = -1;
        for (int i = 0; i < (int)projects.size(); ++i) {
            const bool selected = i == selected_project_idx;
            if (ImGui::Selectable(projects[(size_t)i].filename().string().c_str(), selected)) {
                selected_project_idx = i;
                std::snprintf(project_path_buffer, sizeof(project_path_buffer), "%s", projects[(size_t)i].filename().string().c_str());
            }
        }
        ImGui::BeginDisabled(selected_project_idx < 0 || selected_project_idx >= (int)projects.size());
        if (ImGui::Button("Reload Selected")) {
            std::string message;
            loadProjectFile(ctx, projects[(size_t)selected_project_idx], message);
            if (ctx.project_status) *ctx.project_status = message;
        }
        ImGui::EndDisabled();
    }
    if (ctx.project_status && !ctx.project_status->empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", ctx.project_status->c_str());
    }
    ImGui::EndTabItem();
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
        if (ImGui::Checkbox("Show source under title", &show_source)) {
            ctx.app_settings->map_title_show_primary_parcel_source = show_source;
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        const std::vector<size_t> source_candidates =
            ctx.layers ? mapTitleSourceLayerCandidates(*ctx.layers, ctx.parcel_layer_idx) : std::vector<size_t>{};
        size_t selected_source_idx = ctx.layers
            ? resolveMapTitleSourceLayerIndex(*ctx.layers, ctx.app_settings->map_title_source_layer_file, ctx.parcel_layer_idx)
            : (size_t)-1;
        int selected_combo_idx = -1;
        for (size_t i = 0; i < source_candidates.size(); ++i) {
            if (source_candidates[i] == selected_source_idx) {
                selected_combo_idx = (int)i;
                break;
            }
        }
        ImGui::BeginDisabled(!show_source || source_candidates.empty());
        const char* preview_value = "No visible source layers";
        std::string selected_name;
        if (selected_combo_idx >= 0 && (size_t)selected_combo_idx < source_candidates.size()) {
            selected_name = mapTitleSourceDisplayName((*ctx.layers)[source_candidates[(size_t)selected_combo_idx]]);
            preview_value = selected_name.c_str();
        }
        if (ImGui::BeginCombo("Source layer", preview_value)) {
            for (size_t i = 0; i < source_candidates.size(); ++i) {
                const size_t layer_idx = source_candidates[i];
                const std::string label = mapTitleSourceDisplayName((*ctx.layers)[layer_idx]);
                const bool selected = (int)i == selected_combo_idx;
                if (ImGui::Selectable(label.c_str(), selected)) {
                    ctx.app_settings->map_title_source_layer_file = (*ctx.layers)[layer_idx].file;
                    saveAppSettings(*ctx.root, *ctx.app_settings);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
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
        const std::string preview_source =
            ctx.layers && selected_source_idx != (size_t)-1 && selected_source_idx < ctx.layers->size()
                ? mapTitleSourceLabelForLayer((*ctx.layers)[selected_source_idx])
                : std::string();
        if (!preview_source.empty()) {
            ImGui::TextDisabled("Preview: Source: %s", preview_source.c_str());
        } else {
            ImGui::TextDisabled("Preview: no visible source layer selected");
        }
    }
    ImGui::EndTabItem();
}

void drawRoadLabelsTab(const RightPanelContext& ctx) {
    ImGuiTabItemFlags tab_flags = ImGuiTabItemFlags_None;
    if (ctx.road_label_state && ctx.road_label_state->inspector_open_requested) {
        tab_flags |= ImGuiTabItemFlags_SetSelected;
    }
    if (!ImGui::BeginTabItem("Road Labels", nullptr, tab_flags)) return;
    if (!ctx.road_label_state) {
        ImGui::TextDisabled("Road label state unavailable.");
        ImGui::EndTabItem();
        return;
    }

    RoadLabelState& state = *ctx.road_label_state;
    state.inspector_open_requested = false;
    if (ctx.app_settings && ctx.root) {
        ImGui::SeparatorText("Global Settings");
        bool road_label_click_mode = ctx.app_settings->road_label_click_mode;
        if (ImGui::Checkbox("Road label click mode", &road_label_click_mode)) {
            ctx.app_settings->road_label_click_mode = road_label_click_mode;
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        float road_label_pick_tolerance = ctx.app_settings->road_label_pick_tolerance_px;
        if (ImGui::SliderFloat("Pick tolerance", &road_label_pick_tolerance, 4.0f, 32.0f, "%.0f px")) {
            ctx.app_settings->road_label_pick_tolerance_px = std::clamp(road_label_pick_tolerance, 4.0f, 32.0f);
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        float road_label_size_scale = ctx.app_settings->road_label_size_scale;
        if (ImGui::SliderFloat("Label size", &road_label_size_scale, 0.65f, 2.5f, "%.2fx")) {
            ctx.app_settings->road_label_size_scale = std::clamp(road_label_size_scale, 0.65f, 2.5f);
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        bool road_label_angle_along_road = ctx.app_settings->road_label_angle_along_road;
        if (ImGui::Checkbox("Angle labels along road", &road_label_angle_along_road)) {
            ctx.app_settings->road_label_angle_along_road = road_label_angle_along_road;
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        bool road_label_avoid_overlap = ctx.app_settings->road_label_avoid_overlap;
        if (ImGui::Checkbox("Avoid overlap", &road_label_avoid_overlap)) {
            ctx.app_settings->road_label_avoid_overlap = road_label_avoid_overlap;
            if (!road_label_avoid_overlap) ctx.app_settings->road_label_collision_mode = 0;
            else if (ctx.app_settings->road_label_collision_mode == 0) ctx.app_settings->road_label_collision_mode = 1;
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        const char* collision_modes[] = {"Off", "Compact", "Balanced", "Strict"};
        int collision_mode = ctx.app_settings->road_label_avoid_overlap
            ? std::clamp(ctx.app_settings->road_label_collision_mode, 1, 3)
            : 0;
        if (ImGui::Combo("Collision behavior", &collision_mode, collision_modes, IM_ARRAYSIZE(collision_modes))) {
            ctx.app_settings->road_label_collision_mode = collision_mode;
            ctx.app_settings->road_label_avoid_overlap = collision_mode != 0;
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        float road_label_separation = ctx.app_settings->road_label_separation_px;
        if (ImGui::SliderFloat("Collision separation", &road_label_separation, 0.0f, 48.0f, "%.0f px")) {
            ctx.app_settings->road_label_separation_px = std::clamp(road_label_separation, 0.0f, 48.0f);
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
    } else {
        ImGui::TextDisabled("Road label app settings unavailable.");
    }

    ImGui::SeparatorText("Placed Labels");
    bool visible = state.visible;
    if (ImGui::Checkbox("Visible", &visible)) {
        state.visible = visible;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu labels", state.selections.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(state.selections.empty());
    if (ImGui::Button("Clear All")) {
        state.selections.clear();
        state.selected_idx = (size_t)-1;
    }
    ImGui::EndDisabled();

    if (state.selections.empty()) {
        state.selected_idx = (size_t)-1;
        ImGui::TextDisabled("No road labels are selected.");
        ImGui::TextWrapped("Enable road label click mode, then click roads on the map to add labels.");
        ImGui::EndTabItem();
        return;
    }

    if (state.selected_idx == (size_t)-1 || state.selected_idx >= state.selections.size()) {
        state.selected_idx = 0;
    }
    int selected_label_idx = (int)state.selected_idx;

    ImGui::Separator();
    ImGui::BeginChild("road_label_selection_list", ImVec2(0.0f, 150.0f), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (size_t i = 0; i < state.selections.size(); ++i) {
        const RoadLabelSelection& label = state.selections[i];
        std::string row = label.label.empty() ? std::string("Unnamed road") : label.label;
        row += "##road_label_" + std::to_string(i);
        if (ImGui::Selectable(row.c_str(), selected_label_idx == (int)i)) {
            selected_label_idx = (int)i;
            state.selected_idx = i;
        }
    }
    ImGui::EndChild();

    state.selected_idx = (size_t)std::clamp(selected_label_idx, 0, (int)state.selections.size() - 1);
    RoadLabelSelection& label = state.selections[state.selected_idx];
    ImGui::Text("Selected: %s", label.label.empty() ? "Unnamed road" : label.label.c_str());
    ImGui::TextDisabled("Layer %d, feature %zu", label.layer_idx, label.feature_idx);
    ImGui::TextDisabled("Anchor %.6f, %.6f", label.anchor_lonlat.x, label.anchor_lonlat.y);

    constexpr float pi = 3.14159265358979323846f;
    ImGui::SeparatorText("Selected Label");
    float angle_deg = label.angle_rad * 180.0f / pi;
    if (ImGui::SliderFloat("Rotation", &angle_deg, -180.0f, 180.0f, "%.1f deg")) {
        label.angle_rad = angle_deg * pi / 180.0f;
    }
    if (ImGui::Button("Rotate -15 deg")) {
        label.angle_rad -= 15.0f * pi / 180.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        label.angle_rad = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Rotate 15 deg")) {
        label.angle_rad += 15.0f * pi / 180.0f;
    }
    if (ImGui::Button("Delete Selected")) {
        state.selections.erase(state.selections.begin() + (std::vector<RoadLabelSelection>::difference_type)state.selected_idx);
        if (!state.selections.empty()) {
            state.selected_idx = std::min(state.selected_idx, state.selections.size() - 1);
        } else {
            state.selected_idx = (size_t)-1;
        }
    }
    ImGui::TextDisabled("Rotation persists with the project road label selection.");
    ImGui::EndTabItem();
}

void drawAvTab(const RightPanelContext& ctx) {
    if (!ImGui::BeginTabItem("AV")) return;
    AvCaptureState* state = ctx.av_capture_state;
    if (!state) {
        ImGui::TextDisabled("AV capture state unavailable.");
        ImGui::EndTabItem();
        return;
    }

    if (state->audio_sources.empty()) refreshAvAudioSources(*state);
    if (state->encoder_name.empty()) state->encoder_name = detectAvHardwareEncoder();

    ImGui::Text("Video");
    ImGui::BeginDisabled(state->recording);
    int fps = state->framerate;
    if (ImGui::SliderInt("Frame rate", &fps, 10, 120)) state->framerate = std::clamp(fps, 10, 120);
    int bitrate = state->video_bitrate_mbps;
    if (ImGui::SliderInt("Bitrate Mbps", &bitrate, 2, 80)) {
        state->video_bitrate_mbps = std::clamp(bitrate, 2, 80);
    }

    const char* encoders[] = {"Auto hardware", "h264_nvenc", "h264_qsv", "h264_vaapi", "libx264"};
    int encoder_idx = 0;
    for (int i = 1; i < IM_ARRAYSIZE(encoders); ++i) {
        if (state->encoder_name == encoders[i]) encoder_idx = i;
    }
    if (ImGui::Combo("Encoder", &encoder_idx, encoders, IM_ARRAYSIZE(encoders))) {
        state->encoder_name = encoder_idx == 0 ? detectAvHardwareEncoder() : encoders[encoder_idx];
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Text("Audio");
    bool include_audio = state->include_audio;
    if (ImGui::Checkbox("Record audio", &include_audio)) state->include_audio = include_audio;
    ImGui::SameLine();
    if (ImGui::Button("Refresh sources")) refreshAvAudioSources(*state);

    ImGui::BeginDisabled(state->recording || !state->include_audio);
    std::string preview = "default";
    if (state->selected_audio_source_idx >= 0 &&
        static_cast<size_t>(state->selected_audio_source_idx) < state->audio_sources.size()) {
        preview = state->audio_sources[static_cast<size_t>(state->selected_audio_source_idx)].description;
    }
    if (ImGui::BeginCombo("Audio source", preview.c_str())) {
        for (size_t i = 0; i < state->audio_sources.size(); ++i) {
            const bool selected = static_cast<int>(i) == state->selected_audio_source_idx;
            if (ImGui::Selectable(state->audio_sources[i].description.c_str(), selected)) {
                state->selected_audio_source_idx = static_cast<int>(i);
                state->selected_audio_source = state->audio_sources[i].name;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Text("Status");
    ImGui::TextWrapped("%s", state->status.empty() ? "Idle." : state->status.c_str());
    if (!state->output_path.empty()) ImGui::TextWrapped("Output: %s", state->output_path.string().c_str());
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
    auto select_parcel_ids = [&](const std::vector<std::string>& entity_ids) -> size_t {
        if (ctx.parcel_layer_idx < 0 || (size_t)ctx.parcel_layer_idx >= ctx.layers->size()) return 0;
        clearParcelSelection(*ctx.parcel_selection);
        size_t selected = 0;
        for (const std::string& entity_id : entity_ids) {
            if (entity_id.empty()) continue;
            const UnifiedParcelRecord* rec = ctx.unified_parcels
                ? unifiedParcelAt(*ctx.unified_parcels, entity_id)
                : nullptr;
            const std::string geometry_entity_id =
                rec && !rec->parcel_geometry_entity_id.empty()
                    ? rec->parcel_geometry_entity_id
                    : entity_id;
            if (selectParcel(
                    *ctx.parcel_selection,
                    ctx.parcel_layer_idx,
                    rec ? rec->parcel_local_feature_idx : (size_t)-1,
                    geometry_entity_id,
                    entity_id,
                    true)) {
                selected++;
            }
        }
        ctx.parcel_selection->group_highlight = selected > 0;
        *ctx.show_selected_zone_details = false;
        *ctx.selected_zone_idx = (size_t)-1;
        return selected;
    };

    if (ctx.layers) {
        reconcileParcelSelection(*ctx.parcel_selection, *ctx.layers);
    } else {
        clear_parcel_selection();
    }
    reloadLastProjectAtStartup(ctx);

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
        drawAvTab(ctx);

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
            select_parcel_id,
            select_parcel_ids
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
        drawProjectsTab(ctx);
        drawMapTitleTab(ctx);
        drawRoadLabelsTab(ctx);
        ImGui::EndTabBar();
    }
    autosaveCurrentProjectIfChanged(ctx);
    ImGui::End();
}
