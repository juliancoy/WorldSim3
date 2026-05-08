#pragma once

#include "layer_runtime.h"
#include "profiling.h"
#include "screenshot_state.h"
#include "time_cube.h"
#include "types.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct StatusApiContext {
    const char* app_version = nullptr;
    int protocol_version = 0;
    size_t tile_cache_max = 0;

    std::atomic<bool>* stop = nullptr;
    std::vector<LayerDef>* layers = nullptr;
    TimeCubeService* time_cube_service = nullptr;
    ScreenshotRequestState* screenshot = nullptr;

    std::mutex* status_mutex = nullptr;
    std::vector<LayerRuntimeState>* layer_states = nullptr;
    std::mutex* layer_fill_mutex = nullptr;
    std::vector<bool>* layer_fill_enabled = nullptr;
    std::vector<bool>* layer_hover_enabled = nullptr;
    std::vector<bool>* layer_inspect_enabled = nullptr;
    std::vector<bool>* layer_heatmap_enabled = nullptr;
    std::chrono::steady_clock::time_point* hydration_started_at = nullptr;

    std::atomic<size_t>* hydrated_count = nullptr;
    std::atomic<size_t>* triangulated_count = nullptr;
    std::atomic<size_t>* prof_tile_cache_size = nullptr;
    std::atomic<int>* current_zoom_state = nullptr;
    std::atomic<double>* current_lon_state = nullptr;
    std::atomic<double>* current_lat_state = nullptr;
    std::atomic<size_t>* visible_vacant_parcels_last_frame = nullptr;
    std::atomic<size_t>* vacant_parcels_matched_total = nullptr;
    std::atomic<size_t>* vacant_parcels_with_geometry_total = nullptr;
    std::atomic<size_t>* vacant_parcels_triangulated_renderable_total = nullptr;

    std::atomic<double>* perf_frame_ms_avg = nullptr;
    std::atomic<double>* perf_frame_ms_last = nullptr;
    std::atomic<double>* perf_fps_avg = nullptr;
    std::atomic<double>* ui_left_panel_frac = nullptr;
    std::atomic<double>* ui_right_panel_frac = nullptr;
    std::atomic<size_t>* render_fill_attempts_last_frame = nullptr;
    std::atomic<size_t>* render_fill_success_last_frame = nullptr;
    std::atomic<size_t>* render_fill_no_triangles_last_frame = nullptr;
    std::atomic<size_t>* render_fill_bad_indices_last_frame = nullptr;

    std::mutex* api_layer_mutex = nullptr;
    std::unordered_map<std::string, bool>* api_layer_enable_cmds = nullptr;
    std::unordered_map<std::string, bool>* api_layer_fill_cmds = nullptr;
    std::atomic<int>* api_zoom_cmd = nullptr;
    std::atomic<double>* api_lon_cmd = nullptr;
    std::atomic<double>* api_lat_cmd = nullptr;

    std::mutex* layer_profile_mutex = nullptr;
    std::vector<LayerProfileSnapshot>* layer_profile_snapshot = nullptr;
    std::mutex* profile_mutex = nullptr;
    std::vector<ProfileFrameSample>* profile_samples = nullptr;
    size_t* profile_sample_pos = nullptr;
    size_t* profile_sample_count = nullptr;
    uint64_t* profile_reset_generation = nullptr;

    std::atomic<double>* prof_ui_ms_last = nullptr;
    std::atomic<double>* prof_owner_ms_last = nullptr;
    std::atomic<double>* prof_tile_ms_last = nullptr;
    std::atomic<double>* prof_layer_ms_last = nullptr;
    std::atomic<double>* prof_heatmap_ms_last = nullptr;
    std::atomic<double>* prof_overlay_ms_last = nullptr;
    std::atomic<double>* prof_present_ms_last = nullptr;
    std::atomic<size_t>* prof_tiles_drawn_last = nullptr;
    std::atomic<size_t>* prof_features_considered_last = nullptr;
    std::atomic<size_t>* prof_features_drawn_last = nullptr;
    std::atomic<size_t>* prof_heat_samples_last = nullptr;
    std::atomic<size_t>* prof_retired_textures = nullptr;
};

std::thread startStatusApiWorker(StatusApiContext ctx);
