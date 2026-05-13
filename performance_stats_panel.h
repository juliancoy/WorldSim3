#pragma once

#include "imgui.h"
#include "lan_discovery.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

struct CacheClearUiState {
    bool clear_cache_all = false;
    bool clear_cache_hydration = false;
    bool clear_cache_triangulation = false;
    bool clear_cache_derived = false;
    bool clear_cache_heatmap_memory = false;
    bool clear_cache_heatmap_disk = false;
    bool clear_cache_tile_memory = false;
    bool clear_cache_tile_disk_presence = false;
    std::string last_cache_clear_msg;
};

struct PerformanceStatsUiContext {
    float layout_margin = 0.0f;
    float layout_h = 0.0f;
    float main_panel_h = 0.0f;
    float layout_gap = 0.0f;
    float left_panel_w = 0.0f;

    size_t layer_count = 0;
    size_t hydrated_now = 0;
    size_t triangulated_now = 0;
    size_t hydrated_pending = 0;
    size_t tri_pending = 0;
    float hydrated_frac = 1.0f;
    float tri_frac = 1.0f;
    double elapsed_s = 0.0;
    double hydrate_idle_s = 0.0;
    double tri_idle_s = 0.0;

    double perf_frame_ms_avg = 0.0;
    double perf_frame_ms_last = 0.0;
    double perf_fps_avg = 0.0;

    size_t data_library_rendered_rows_last = 0;
    size_t data_library_visible_rows = 0;
    size_t data_library_cache_rebuilds = 0;
    size_t people_pay_rendered_rows_last = 0;
    size_t people_pay_visible_rows = 0;
    size_t people_pay_cache_rebuilds = 0;

    size_t render_fill_success_last_frame = 0;
    size_t render_fill_attempts_last_frame = 0;
    size_t render_fill_no_triangles_last_frame = 0;
    size_t render_fill_bad_indices_last_frame = 0;

    char* arkavo_room_id = nullptr;
    size_t arkavo_room_id_size = 0;
    bool arkavo_connected = false;
    std::string arkavo_self_peer_id;
    size_t arkavo_tracked_peers = 0;
    std::vector<std::string> arkavo_open_peers;
    char* arkavo_send_peer = nullptr;
    size_t arkavo_send_peer_size = 0;
    char* arkavo_send_path = nullptr;
    size_t arkavo_send_path_size = 0;
    std::string* arkavo_status = nullptr;
    std::string* arkavo_err = nullptr;
    std::function<void()> on_connect_arkavo;
    std::function<void()> on_disconnect_arkavo;
    std::function<void()> on_send_arkavo_file;

    LanDiscoveryPanelContext* lan_panel = nullptr;

    size_t tile_cache_size = 0;
    size_t max_tile_cache = 0;
    CacheClearUiState* cache_clear = nullptr;
    std::function<void()> on_clear_cache;
};

void drawPerformanceStatsPanel(PerformanceStatsUiContext& ctx);
