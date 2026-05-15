#pragma once

#include "app_frame_support.h"
#include "arkavo_realtime_client.h"
#include "arkavo_rtc_session_manager.h"
#include "performance_stats_panel.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct PerformanceRuntimeContext {
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
    std::atomic<double>* perf_frame_ms_avg = nullptr;
    std::atomic<double>* perf_frame_ms_last = nullptr;
    std::atomic<double>* perf_fps_avg = nullptr;
    size_t data_library_rendered_rows_last = 0;
    size_t data_library_visible_rows = 0;
    size_t data_library_cache_rebuilds = 0;
    size_t people_pay_rendered_rows_last = 0;
    size_t people_pay_visible_rows = 0;
    size_t people_pay_cache_rebuilds = 0;
    std::atomic<size_t>* render_fill_success_last_frame = nullptr;
    std::atomic<size_t>* render_fill_attempts_last_frame = nullptr;
    std::atomic<size_t>* render_fill_no_triangles_last_frame = nullptr;
    std::atomic<size_t>* render_fill_bad_indices_last_frame = nullptr;
    LanDiscoveryContext* lan_discovery = nullptr;
    std::vector<LanPeerInfo>* lan_peers = nullptr;
    std::string* lan_scan_status = nullptr;
    char* arkavo_room_id = nullptr;
    size_t arkavo_room_id_size = 0;
    std::string* arkavo_status = nullptr;
    std::string* arkavo_err = nullptr;
    std::unique_ptr<ArkavoRealtimeClient>* arkavo_client = nullptr;
    std::unique_ptr<ArkavoRtcSessionManager>* arkavo_rtc = nullptr;
    char* arkavo_send_peer = nullptr;
    size_t arkavo_send_peer_size = 0;
    char* arkavo_send_path = nullptr;
    size_t arkavo_send_path_size = 0;
    bool* clear_cache_all = nullptr;
    bool* clear_cache_hydration = nullptr;
    bool* clear_cache_triangulation = nullptr;
    bool* clear_cache_derived = nullptr;
    bool* clear_cache_heatmap_memory = nullptr;
    bool* clear_cache_heatmap_disk = nullptr;
    bool* clear_cache_tile_memory = nullptr;
    bool* clear_cache_tile_disk_presence = nullptr;
    std::string* last_cache_clear_msg = nullptr;
    const std::filesystem::path* cache_hydration_dir = nullptr;
    const std::filesystem::path* cache_triangulation_dir = nullptr;
    const std::filesystem::path* cache_derived_dir = nullptr;
    const std::filesystem::path* cache_aggregate_dir = nullptr;
    std::vector<LayerDef>* layers = nullptr;
    std::vector<LayerSpatialIndex>* layer_spatial = nullptr;
    std::vector<size_t>* layer_fallback_scan_cursor = nullptr;
    std::vector<LayerProfileAccumulator>* layer_profile_accumulators = nullptr;
    std::vector<bool>* layer_profile_dirty = nullptr;
    std::vector<LayerRuntimeState>* layer_states = nullptr;
    std::mutex* hydrated_mutex = nullptr;
    std::deque<HydratedLayer>* hydrated_queue = nullptr;
    std::mutex* tri_mutex = nullptr;
    std::deque<TriJob>* tri_jobs = nullptr;
    std::deque<TriResult>* tri_results = nullptr;
    std::condition_variable* tri_cv = nullptr;
    std::mutex* spatial_mutex = nullptr;
    std::deque<SpatialIndexJob>* spatial_jobs = nullptr;
    std::deque<SpatialIndexResult>* spatial_results = nullptr;
    std::condition_variable* spatial_cv = nullptr;
    std::vector<size_t>* spatial_index_requested_feature_count = nullptr;
    std::vector<std::string>* spatial_index_requested_signature = nullptr;
    std::mutex* hydrate_req_mutex = nullptr;
    std::deque<size_t>* hydrate_requests = nullptr;
    std::vector<bool>* hydration_requested = nullptr;
    std::vector<bool>* hydration_required = nullptr;
    std::mutex* status_mutex = nullptr;
    int parcel_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    std::atomic<size_t>* hydrated_count = nullptr;
    std::atomic<size_t>* triangulated_count = nullptr;
    std::function<void(size_t, bool)> enqueue_hydration;
    std::function<size_t(const std::filesystem::path&)> clear_cache_tree;
    std::function<void()> clear_heatmap_runtime_cache;
    std::function<void()> reset_derived_cache_state;
    std::function<void()> trim_process_heap;
    std::function<void()> clear_tile_disk_presence_cache;
};

void runPerformanceRuntimeSupport(const PerformanceRuntimeContext& ctx);
