#pragma once

#include "filters.h"
#include "layer_runtime_coordinator.h"
#include "status_api.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct ApiControlContext {
    int* zoom = nullptr;
    double* center_lon = nullptr;
    double* center_lat = nullptr;
    std::atomic<int>* api_zoom_cmd = nullptr;
    std::atomic<double>* api_lon_cmd = nullptr;
    std::atomic<double>* api_lat_cmd = nullptr;
    int min_zoom = 0;
    int max_zoom = 0;

    LayerApiCommandCoordinatorContext* layer_api = nullptr;

    std::atomic<uint64_t>* api_ui_cmd_seq = nullptr;
    std::atomic<int>* api_ui_cmd_kind = nullptr;
    std::atomic<double>* api_ui_cmd_x = nullptr;
    std::atomic<double>* api_ui_cmd_y = nullptr;
    std::atomic<int>* api_ui_cmd_button = nullptr;
    std::atomic<double>* api_ui_cmd_scroll_y = nullptr;
    uint64_t* api_ui_cmd_last_seq = nullptr;
    bool* api_ui_mouse_release_pending = nullptr;
    int* api_ui_mouse_release_button = nullptr;

    MapFilterState* map_filter_state = nullptr;
    FilterResultSet* active_filter_result_set = nullptr;
    std::vector<QueryMapLayer>* query_layers = nullptr;
    std::string* active_filter_status = nullptr;
    std::mutex* api_control_mutex = nullptr;
    ApiFilterControlCommand* api_filter_control_cmd = nullptr;
    std::vector<ApiQueryControlCommand>* api_query_control_cmds = nullptr;
    uint64_t* filter_state_key = nullptr;
};

void applyApiControlCommands(ApiControlContext& ctx);
