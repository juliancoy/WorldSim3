#include "api_control_commands.h"

#include "app_utils.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace {
bool parseBoolControl(const std::string& value) {
    const std::string v = toLowerAscii(trimDisplayValue(value));
    return v == "1" || v == "true" || v == "on" || v == "yes";
}

int parseIntControl(const std::string& value, int fallback) {
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

void copyControlText(char* dst, size_t dst_size, const std::string& value) {
    if (!dst || dst_size == 0) return;
    std::snprintf(dst, dst_size, "%s", value.c_str());
}

void clearFieldFilters(MapFilterState& filters) {
    filters.enabled = false;
    filters.use_date = false;
    filters.blocklot[0] = '\0';
    filters.status[0] = '\0';
    filters.address[0] = '\0';
    filters.owner[0] = '\0';
    filters.zip[0] = '\0';
    filters.crime = CrimeFilterState{};
}

void applyFilterCommand(MapFilterState& filters, const ApiFilterControlCommand& cmd) {
    if (cmd.clear_fields) clearFieldFilters(filters);
    if (cmd.clear_selected_owners) filters.selected_owners.clear();

    auto has = [&](const char* key) {
        return cmd.values.find(key) != cmd.values.end();
    };
    auto get = [&](const char* key) -> const std::string& {
        return cmd.values.find(key)->second;
    };

    if (has("enabled")) filters.enabled = parseBoolControl(get("enabled"));
    if (has("use_date")) filters.use_date = parseBoolControl(get("use_date"));
    if (has("year_min")) filters.year_min = parseIntControl(get("year_min"), filters.year_min);
    if (has("year_max")) filters.year_max = parseIntControl(get("year_max"), filters.year_max);
    if (filters.year_min > filters.year_max) std::swap(filters.year_min, filters.year_max);
    if (has("blocklot")) copyControlText(filters.blocklot, sizeof(filters.blocklot), get("blocklot"));
    if (has("status")) copyControlText(filters.status, sizeof(filters.status), get("status"));
    if (has("address")) copyControlText(filters.address, sizeof(filters.address), get("address"));
    if (has("owner")) copyControlText(filters.owner, sizeof(filters.owner), get("owner"));
    if (has("zip")) copyControlText(filters.zip, sizeof(filters.zip), get("zip"));

    if (has("crime_enabled")) filters.crime.enabled = parseBoolControl(get("crime_enabled"));
    if (has("crime_use_year")) filters.crime.use_year = parseBoolControl(get("crime_use_year"));
    if (has("crime_year_min")) filters.crime.year_min = parseIntControl(get("crime_year_min"), filters.crime.year_min);
    if (has("crime_year_max")) filters.crime.year_max = parseIntControl(get("crime_year_max"), filters.crime.year_max);
    if (filters.crime.year_min > filters.crime.year_max) std::swap(filters.crime.year_min, filters.crime.year_max);
    if (has("crime_homicide")) filters.crime.homicide = parseBoolControl(get("crime_homicide"));
    if (has("crime_robbery")) filters.crime.robbery = parseBoolControl(get("crime_robbery"));
    if (has("crime_assault")) filters.crime.assault = parseBoolControl(get("crime_assault"));
    if (has("crime_burglary")) filters.crime.burglary = parseBoolControl(get("crime_burglary"));
    if (has("crime_theft")) filters.crime.theft = parseBoolControl(get("crime_theft"));
    if (has("crime_auto_theft")) filters.crime.auto_theft = parseBoolControl(get("crime_auto_theft"));
    if (has("crime_drug")) filters.crime.drug = parseBoolControl(get("crime_drug"));
    if (has("crime_shooting")) filters.crime.shooting = parseBoolControl(get("crime_shooting"));
}

void applyQueuedFilterControls(ApiControlContext& ctx) {
    if (!ctx.map_filter_state || !ctx.active_filter_result_set || !ctx.query_layers ||
        !ctx.active_filter_status || !ctx.api_control_mutex || !ctx.api_filter_control_cmd ||
        !ctx.api_query_control_cmds) {
        return;
    }

    ApiFilterControlCommand filter_cmd;
    std::vector<ApiQueryControlCommand> query_cmds;
    {
        std::lock_guard<std::mutex> lk(*ctx.api_control_mutex);
        if (ctx.api_filter_control_cmd->pending) {
            filter_cmd = std::move(*ctx.api_filter_control_cmd);
            *ctx.api_filter_control_cmd = ApiFilterControlCommand{};
        }
        query_cmds.swap(*ctx.api_query_control_cmds);
    }

    bool changed = false;
    if (filter_cmd.pending) {
        applyFilterCommand(*ctx.map_filter_state, filter_cmd);
        if (filter_cmd.clear_query_layers) ctx.query_layers->clear();
        if (filter_cmd.clear_fields) {
            *ctx.active_filter_result_set = FilterResultSet{};
            *ctx.active_filter_status = "REST controls cleared active filters.";
        }
        changed = true;
    }

    for (auto& cmd : query_cmds) {
        if (cmd.apply_mode == ApiQueryControlCommand::ApplyMode::Filter) {
            *ctx.active_filter_result_set = std::move(cmd.layer.result_set);
            ctx.active_filter_result_set->active = true;
            *ctx.active_filter_status = "REST filter: " + cmd.layer.status;
            changed = true;
        } else if (cmd.apply_mode == ApiQueryControlCommand::ApplyMode::FilterLayer) {
            *ctx.active_filter_result_set = cmd.layer.result_set;
            ctx.active_filter_result_set->active = true;
            *ctx.active_filter_status = "REST filter: " + cmd.layer.status;
            ctx.query_layers->push_back(std::move(cmd.layer));
            changed = true;
        } else if (cmd.apply_mode == ApiQueryControlCommand::ApplyMode::Layer) {
            ctx.query_layers->push_back(std::move(cmd.layer));
            changed = true;
        }
    }

    if (changed && ctx.filter_state_key) *ctx.filter_state_key = 0;
}
}

void applyApiControlCommands(ApiControlContext& ctx) {
    if (!ctx.zoom || !ctx.center_lon || !ctx.center_lat ||
        !ctx.api_zoom_cmd || !ctx.api_lon_cmd || !ctx.api_lat_cmd) {
        return;
    }

    int zc = ctx.api_zoom_cmd->exchange(-1, std::memory_order_relaxed);
    if (zc >= ctx.min_zoom && zc <= ctx.max_zoom) *ctx.zoom = zc;
    double lonc = ctx.api_lon_cmd->exchange(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
    double latc = ctx.api_lat_cmd->exchange(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
    if (!std::isnan(lonc)) *ctx.center_lon = lonc;
    if (!std::isnan(latc)) *ctx.center_lat = std::clamp(latc, -85.0, 85.0);

    if (ctx.layer_api) applyLayerApiCommands(*ctx.layer_api);
    applyQueuedFilterControls(ctx);

    if (!ctx.api_ui_cmd_seq || !ctx.api_ui_cmd_kind || !ctx.api_ui_cmd_x || !ctx.api_ui_cmd_y ||
        !ctx.api_ui_cmd_button || !ctx.api_ui_cmd_scroll_y || !ctx.api_ui_cmd_last_seq ||
        !ctx.api_ui_mouse_release_pending || !ctx.api_ui_mouse_release_button) {
        return;
    }

    const uint64_t ui_seq = ctx.api_ui_cmd_seq->load(std::memory_order_relaxed);
    ImGuiIO& io = ImGui::GetIO();
    if (*ctx.api_ui_mouse_release_pending) {
        io.AddMouseButtonEvent(std::clamp(*ctx.api_ui_mouse_release_button, 0, 4), false);
        *ctx.api_ui_mouse_release_pending = false;
    }
    if (ui_seq != *ctx.api_ui_cmd_last_seq) {
        *ctx.api_ui_cmd_last_seq = ui_seq;
        const int kind = ctx.api_ui_cmd_kind->load(std::memory_order_relaxed);
        const float x = (float)ctx.api_ui_cmd_x->load(std::memory_order_relaxed);
        const float y = (float)ctx.api_ui_cmd_y->load(std::memory_order_relaxed);
        const int button = std::clamp(ctx.api_ui_cmd_button->load(std::memory_order_relaxed), 0, 4);
        const float scroll_y = (float)ctx.api_ui_cmd_scroll_y->load(std::memory_order_relaxed);
        if (kind == 1) {
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(button, true);
            *ctx.api_ui_mouse_release_pending = true;
            *ctx.api_ui_mouse_release_button = button;
        } else if (kind == 2) {
            io.AddMousePosEvent(x, y);
        } else if (kind == 3) {
            io.AddMouseWheelEvent(0.0f, scroll_y);
        }
    }
}
