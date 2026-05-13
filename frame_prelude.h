#pragma once

#include "api_control_commands.h"
#include "basemap_coverage.h"
#include "data_library_coordinator.h"
#include "lan_discovery.h"
#include "layer_download_queue.h"
#include "profiling_layer_snapshot.h"
#include "tiles.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct FramePreludeResult {
    LayerProfileSnapshotRefreshContext layer_profile_snapshot;
    LanDiscoveryContext lan_discovery;
    LayerDownloadQueueContext layer_download;
    std::function<size_t()> queue_all_missing_layer_downloads;
};

struct FramePreludeContext {
    const std::filesystem::path* root = nullptr;
    std::vector<LayerDef>* layers = nullptr;
    std::vector<LayerSpatialIndex>* layer_spatial = nullptr;
    std::vector<bool>* layer_profile_dirty = nullptr;
    std::vector<LayerProfileSnapshot>* layer_profile_snapshot = nullptr;
    std::mutex* layer_profile_mutex = nullptr;
    LayerRegistry* layer_registry = nullptr;
    std::vector<bool>* local_layer_exists_cache = nullptr;
    std::vector<FreshnessState>* data_freshness_state = nullptr;
    std::vector<std::string>* data_freshness_msg = nullptr;
    std::string* data_library_status_msg = nullptr;
    std::deque<size_t>* layer_download_queue = nullptr;
    bool* layer_download_inflight = nullptr;
    size_t* layer_download_active_idx = nullptr;
    std::future<VersionedDownloadResult>* layer_download_future = nullptr;
    std::string* layer_download_active_file = nullptr;
    std::string* layer_download_last_event = nullptr;
    bool* layer_download_queue_loaded = nullptr;
    std::vector<LanPeerInfo>* lan_peers = nullptr;
    std::string* lan_scan_status = nullptr;
    std::chrono::steady_clock::time_point* lan_last_scan_at = nullptr;
    int protocol_version = 0;
    double* center_lon = nullptr;
    double* center_lat = nullptr;
    int* zoom = nullptr;
    int min_zoom = 0;
    int max_zoom = 0;
    std::atomic<int>* current_zoom_state = nullptr;
    std::atomic<double>* current_lon_state = nullptr;
    std::atomic<double>* current_lat_state = nullptr;
    std::mutex* api_layer_mutex = nullptr;
    std::unordered_map<std::string, bool>* api_layer_enable_cmds = nullptr;
    std::unordered_map<std::string, bool>* api_layer_fill_cmds = nullptr;
    std::vector<std::string>* api_layer_download_cmds = nullptr;
    std::mutex* layer_fill_mutex = nullptr;
    std::vector<bool>* layer_fill_enabled = nullptr;
    bool* layer_fill_state_changed = nullptr;
    std::atomic<uint64_t>* api_ui_cmd_seq = nullptr;
    std::atomic<int>* api_ui_cmd_kind = nullptr;
    std::atomic<double>* api_ui_cmd_x = nullptr;
    std::atomic<double>* api_ui_cmd_y = nullptr;
    std::atomic<int>* api_ui_cmd_button = nullptr;
    std::atomic<double>* api_ui_cmd_scroll_y = nullptr;
    uint64_t* api_ui_cmd_last_seq = nullptr;
    bool* api_ui_mouse_release_pending = nullptr;
    int* api_ui_mouse_release_button = nullptr;
    std::atomic<int>* api_zoom_cmd = nullptr;
    std::atomic<double>* api_lon_cmd = nullptr;
    std::atomic<double>* api_lat_cmd = nullptr;
    LazyTileDownloadState* lazy_tile_download = nullptr;
    bool* basemap_coverage_dirty = nullptr;
    std::chrono::steady_clock::time_point* basemap_availability_last_check = nullptr;
    size_t* osm_missing_tiles_cached = nullptr;
    size_t* osm_total_tiles_cached = nullptr;
    size_t* topo_missing_tiles_cached = nullptr;
    size_t* topo_total_tiles_cached = nullptr;
    bool* basemap_source_has_any_files_cached = nullptr;
    bool* topo_tiles_available_cached = nullptr;
    bool* topo_vector_available_cached = nullptr;
    int min_tile_zoom = 0;
    int max_native_tile_zoom = 0;
    std::chrono::steady_clock::duration basemap_coverage_refresh_interval{};
    std::function<void(size_t, bool)> mark_local_layer_exists;
    std::function<void(size_t, bool)> enqueue_hydration;
    std::function<void()> refresh_local_layer_exists_cache;
};

FramePreludeResult runFramePrelude(const FramePreludeContext& ctx);
