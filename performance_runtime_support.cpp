#include "performance_runtime_support.h"

#include "app_utils.h"
#include "arkavo_signaling_transport_curl.h"
#include "memory_utils.h"
#include "worldsim_app_internal.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

void runPerformanceRuntimeSupport(const PerformanceRuntimeContext& ctx) {
    if (!ctx.render_fill_success_last_frame || !ctx.render_fill_attempts_last_frame ||
        !ctx.render_fill_no_triangles_last_frame || !ctx.render_fill_bad_indices_last_frame ||
        !ctx.lan_discovery || !ctx.lan_peers || !ctx.lan_scan_status || !ctx.arkavo_room_id ||
        !ctx.arkavo_status || !ctx.arkavo_err || !ctx.arkavo_client || !ctx.arkavo_rtc ||
        !ctx.arkavo_send_peer || !ctx.arkavo_send_path || !ctx.clear_cache_all ||
        !ctx.clear_cache_hydration || !ctx.clear_cache_triangulation || !ctx.clear_cache_derived ||
        !ctx.clear_cache_heatmap_memory || !ctx.clear_cache_heatmap_disk ||
        !ctx.clear_cache_tile_memory || !ctx.clear_cache_tile_disk_presence ||
        !ctx.last_cache_clear_msg || !ctx.cache_hydration_dir || !ctx.cache_triangulation_dir ||
        !ctx.cache_derived_dir || !ctx.cache_aggregate_dir || !ctx.layers || !ctx.layer_spatial ||
        !ctx.layer_profile_dirty || !ctx.layer_states || !ctx.hydrated_mutex ||
        !ctx.hydrated_queue || !ctx.tri_mutex || !ctx.tri_jobs || !ctx.tri_results ||
        !ctx.tri_cv || !ctx.hydrate_req_mutex || !ctx.hydrate_requests ||
        !ctx.hydration_requested || !ctx.hydration_required || !ctx.status_mutex ||
        !ctx.hydrated_count || !ctx.triangulated_count) {
        return;
    }

    LanDiscoveryPanelContext lan_panel_ctx;
    lan_panel_ctx.discovery = ctx.lan_discovery;
    lan_panel_ctx.peers = ctx.lan_peers;
    lan_panel_ctx.scan_status = ctx.lan_scan_status;
    std::vector<std::string> arkavo_open_peers;
    if (*ctx.arkavo_rtc) arkavo_open_peers = (*ctx.arkavo_rtc)->connectedPeers();

    CacheClearUiState cache_clear_ui;
    cache_clear_ui.clear_cache_all = *ctx.clear_cache_all;
    cache_clear_ui.clear_cache_hydration = *ctx.clear_cache_hydration;
    cache_clear_ui.clear_cache_triangulation = *ctx.clear_cache_triangulation;
    cache_clear_ui.clear_cache_derived = *ctx.clear_cache_derived;
    cache_clear_ui.clear_cache_heatmap_memory = *ctx.clear_cache_heatmap_memory;
    cache_clear_ui.clear_cache_heatmap_disk = *ctx.clear_cache_heatmap_disk;
    cache_clear_ui.clear_cache_tile_memory = *ctx.clear_cache_tile_memory;
    cache_clear_ui.clear_cache_tile_disk_presence = *ctx.clear_cache_tile_disk_presence;
    cache_clear_ui.last_cache_clear_msg = *ctx.last_cache_clear_msg;

    auto connect_arkavo = [&]() {
        ArkavoRealtimeClient::Config cfg;
        cfg.room_id = trimDisplayValue(ctx.arkavo_room_id);
        cfg.signaling_url = "wss://signaling.arkavo.org/";
        auto transport = std::make_unique<ArkavoSignalingTransportCurl>();
        *ctx.arkavo_client = std::make_unique<ArkavoRealtimeClient>(cfg, std::move(transport));
        *ctx.arkavo_rtc = std::make_unique<ArkavoRtcSessionManager>(**ctx.arkavo_client);
        (*ctx.arkavo_rtc)->on_log = [&](const std::string& m) { *ctx.arkavo_status = m; };
        (*ctx.arkavo_rtc)->on_error = [&](const std::string& e) { *ctx.arkavo_err = e; };
        (*ctx.arkavo_rtc)->on_file_received = [&](const std::string& peer, const std::filesystem::path& p) {
            *ctx.arkavo_status = "received file from " + peer + ": " + p.string();
        };
        (*ctx.arkavo_client)->on_log = [&](const std::string& m) { *ctx.arkavo_status = m; };
        (*ctx.arkavo_client)->on_error = [&](const std::string& e) { *ctx.arkavo_err = e; };
        (*ctx.arkavo_client)->on_peer_should_connect = [&](const std::string& peer_id, bool initiator) {
            if (*ctx.arkavo_rtc) (*ctx.arkavo_rtc)->connectPeer(peer_id, initiator);
        };
        (*ctx.arkavo_client)->on_peer_left = [&](const std::string& peer_id) {
            if (*ctx.arkavo_rtc) (*ctx.arkavo_rtc)->removePeer(peer_id);
        };
        (*ctx.arkavo_client)->on_signal_payload = [&](const std::string& peer_id, const nlohmann::json& payload) {
            if (*ctx.arkavo_rtc) (*ctx.arkavo_rtc)->handleSignal(peer_id, payload);
        };
        std::string err;
        if (!(*ctx.arkavo_client)->start(err)) {
            *ctx.arkavo_err = err;
            *ctx.arkavo_status = "connect failed";
        } else {
            *ctx.arkavo_status = "connecting";
        }
    };
    auto disconnect_arkavo = [&]() {
        if (*ctx.arkavo_rtc) (*ctx.arkavo_rtc)->closeAll();
        if (*ctx.arkavo_client) (*ctx.arkavo_client)->stop();
        ctx.arkavo_rtc->reset();
        ctx.arkavo_client->reset();
        *ctx.arkavo_status = "disconnected";
    };
    auto send_arkavo_file = [&]() {
        if (!*ctx.arkavo_rtc) return;
        std::string err;
        if (!(*ctx.arkavo_rtc)->sendFile(trimDisplayValue(ctx.arkavo_send_peer), trimDisplayValue(ctx.arkavo_send_path), err)) {
            *ctx.arkavo_err = err;
        } else {
            *ctx.arkavo_status = "file send queued";
        }
    };
    auto clear_cache_action = [&]() {
        const bool has_any_selected =
            cache_clear_ui.clear_cache_hydration ||
            cache_clear_ui.clear_cache_triangulation ||
            cache_clear_ui.clear_cache_derived ||
            cache_clear_ui.clear_cache_heatmap_memory ||
            cache_clear_ui.clear_cache_heatmap_disk ||
            cache_clear_ui.clear_cache_tile_memory ||
            cache_clear_ui.clear_cache_tile_disk_presence;
        if (!has_any_selected) {
            cache_clear_ui.last_cache_clear_msg = "No cache scope selected for clear.";
            return;
        }
        size_t removed_files = 0;
        std::error_code ec;
        std::vector<std::string> cleared_scopes;
        if (cache_clear_ui.clear_cache_hydration) {
            removed_files += ctx.clear_cache_tree(*ctx.cache_hydration_dir);
            cleared_scopes.push_back("hydration");
        }
        if (cache_clear_ui.clear_cache_triangulation) {
            removed_files += ctx.clear_cache_tree(*ctx.cache_triangulation_dir);
            cleared_scopes.push_back("triangulation");
        }
        if (cache_clear_ui.clear_cache_derived) {
            removed_files += ctx.clear_cache_tree(*ctx.cache_derived_dir);
            cleared_scopes.push_back("derived");
        }
        if (cache_clear_ui.clear_cache_heatmap_disk) {
            removed_files += ctx.clear_cache_tree(*ctx.cache_aggregate_dir);
            cleared_scopes.push_back("heatmap-aggregate disk");
        }
        if (cache_clear_ui.clear_cache_heatmap_memory) {
            ctx.clear_heatmap_runtime_cache();
            cleared_scopes.push_back("heatmap-aggregate runtime");
        }
        if (cache_clear_ui.clear_cache_tile_memory) {
            for (auto& kv : g_TileCache) destroyTileTexture(kv.second.tex);
            g_TileCache.clear();
            g_TileLRU.clear();
            cleared_scopes.push_back("tile");
        }
        if (cache_clear_ui.clear_cache_tile_disk_presence) {
            ctx.clear_tile_disk_presence_cache();
            cleared_scopes.push_back("tile disk presence");
        }
        if (cache_clear_ui.clear_cache_hydration) std::filesystem::create_directories(*ctx.cache_hydration_dir, ec);
        if (cache_clear_ui.clear_cache_triangulation) std::filesystem::create_directories(*ctx.cache_triangulation_dir, ec);
        if (cache_clear_ui.clear_cache_derived) std::filesystem::create_directories(*ctx.cache_derived_dir, ec);
        if (cache_clear_ui.clear_cache_heatmap_disk) std::filesystem::create_directories(*ctx.cache_aggregate_dir, ec);
        const bool clear_hydration_data = cache_clear_ui.clear_cache_hydration;
        const bool clear_tri_data = cache_clear_ui.clear_cache_triangulation;
        const bool clear_derived_data = cache_clear_ui.clear_cache_derived || cache_clear_ui.clear_cache_hydration;
        if (clear_hydration_data) {
            {
                std::lock_guard<std::mutex> lk(*ctx.hydrated_mutex);
                ctx.hydrated_queue->clear();
            }
            {
                std::lock_guard<std::mutex> lk(*ctx.tri_mutex);
                ctx.tri_jobs->clear();
                ctx.tri_results->clear();
            }
            {
                std::lock_guard<std::mutex> lk(*ctx.hydrate_req_mutex);
                ctx.hydrate_requests->clear();
                std::fill(ctx.hydration_requested->begin(), ctx.hydration_requested->end(), false);
                std::fill(ctx.hydration_required->begin(), ctx.hydration_required->end(), false);
            }
            for (size_t i = 0; i < ctx.layers->size(); ++i) {
                releaseContainerStorage((*ctx.layers)[i].features);
                (*ctx.layer_spatial)[i] = LayerSpatialIndex{};
                if (i < ctx.layer_profile_dirty->size()) (*ctx.layer_profile_dirty)[i] = true;
            }
            {
                std::lock_guard<std::mutex> lk(*ctx.status_mutex);
                for (size_t i = 0; i < ctx.layers->size(); ++i) {
                    if (i < ctx.layer_states->size()) {
                        (*ctx.layer_states)[i].status = LayerPipelineStatus::Queued;
                        (*ctx.layer_states)[i].feature_count = 0;
                        (*ctx.layer_states)[i].error.clear();
                        (*ctx.layer_states)[i].hydration_source_signature.clear();
                        (*ctx.layer_states)[i].triangulation_source_signature.clear();
                        (*ctx.layer_states)[i].hydration_loaded_from_cache = false;
                    }
                }
            }
            for (size_t i = 0; i < ctx.layers->size(); ++i) {
                if ((*ctx.layers)[i].enabled) ctx.enqueue_hydration(i, false);
            }
            ctx.trim_process_heap();
            ctx.hydrated_count->store(0, std::memory_order_relaxed);
            ctx.triangulated_count->store(0, std::memory_order_relaxed);
        }
        if (clear_tri_data && !clear_hydration_data) {
            {
                std::lock_guard<std::mutex> lk(*ctx.tri_mutex);
                ctx.tri_jobs->clear();
                ctx.tri_results->clear();
            }
            const bool vac_layer_active_now =
                (ctx.vacant_notice_layer_idx >= 0 && (size_t)ctx.vacant_notice_layer_idx < ctx.layers->size() && (*ctx.layers)[(size_t)ctx.vacant_notice_layer_idx].enabled) ||
                (ctx.vacant_rehab_layer_idx >= 0 && (size_t)ctx.vacant_rehab_layer_idx < ctx.layers->size() && (*ctx.layers)[(size_t)ctx.vacant_rehab_layer_idx].enabled);
            for (size_t i = 0; i < ctx.layers->size(); ++i) {
                if ((*ctx.layers)[i].features.empty()) continue;
                const bool parcel_dep_priority = vac_layer_active_now && ctx.parcel_layer_idx >= 0 && (int)i == ctx.parcel_layer_idx;
                if (!(*ctx.layers)[i].enabled && !parcel_dep_priority) continue;
                TriJob tj;
                tj.index = i;
                tj.file = (*ctx.layers)[i].file;
                if (i < ctx.layer_states->size()) {
                    tj.source_signature = (*ctx.layer_states)[i].hydration_source_signature;
                }
                tj.rings_per_feature.reserve((*ctx.layers)[i].features.size());
                for (const auto& fg : (*ctx.layers)[i].features) tj.rings_per_feature.push_back(fg.rings);
                {
                    std::lock_guard<std::mutex> lk2(*ctx.tri_mutex);
                    if (parcel_dep_priority) ctx.tri_jobs->push_front(std::move(tj));
                    else ctx.tri_jobs->push_back(std::move(tj));
                }
                ctx.tri_cv->notify_one();
                {
                    std::lock_guard<std::mutex> lk3(*ctx.status_mutex);
                    if (i < ctx.layer_states->size()) (*ctx.layer_states)[i].status = LayerPipelineStatus::TriQueued;
                }
            }
            ctx.tri_cv->notify_all();
        }
        if (clear_derived_data) {
            ctx.reset_derived_cache_state();
        }
        std::ostringstream clear_msg;
        clear_msg << "Cleared cache scopes: ";
        for (size_t i = 0; i < cleared_scopes.size(); ++i) {
            if (i > 0) clear_msg << ", ";
            clear_msg << cleared_scopes[i];
        }
        clear_msg << " | removed " << removed_files << " files.";
        if (clear_hydration_data) clear_msg << " Rehydrating enabled layers.";
        cache_clear_ui.last_cache_clear_msg = clear_msg.str();
    };

    PerformanceStatsUiContext perf_ui_ctx;
    perf_ui_ctx.layout_margin = ctx.layout_margin;
    perf_ui_ctx.layout_h = ctx.layout_h;
    perf_ui_ctx.main_panel_h = ctx.main_panel_h;
    perf_ui_ctx.layout_gap = ctx.layout_gap;
    perf_ui_ctx.left_panel_w = ctx.left_panel_w;
    perf_ui_ctx.layer_count = ctx.layer_count;
    perf_ui_ctx.hydrated_now = ctx.hydrated_now;
    perf_ui_ctx.triangulated_now = ctx.triangulated_now;
    perf_ui_ctx.hydrated_pending = ctx.hydrated_pending;
    perf_ui_ctx.tri_pending = ctx.tri_pending;
    perf_ui_ctx.hydrated_frac = ctx.hydrated_frac;
    perf_ui_ctx.tri_frac = ctx.tri_frac;
    perf_ui_ctx.elapsed_s = ctx.elapsed_s;
    perf_ui_ctx.hydrate_idle_s = ctx.hydrate_idle_s;
    perf_ui_ctx.tri_idle_s = ctx.tri_idle_s;
    perf_ui_ctx.perf_frame_ms_avg = ctx.perf_frame_ms_avg->load(std::memory_order_relaxed);
    perf_ui_ctx.perf_frame_ms_last = ctx.perf_frame_ms_last->load(std::memory_order_relaxed);
    perf_ui_ctx.perf_fps_avg = ctx.perf_fps_avg->load(std::memory_order_relaxed);
    perf_ui_ctx.data_library_rendered_rows_last = ctx.data_library_rendered_rows_last;
    perf_ui_ctx.data_library_visible_rows = ctx.data_library_visible_rows;
    perf_ui_ctx.data_library_cache_rebuilds = ctx.data_library_cache_rebuilds;
    perf_ui_ctx.people_pay_rendered_rows_last = ctx.people_pay_rendered_rows_last;
    perf_ui_ctx.people_pay_visible_rows = ctx.people_pay_visible_rows;
    perf_ui_ctx.people_pay_cache_rebuilds = ctx.people_pay_cache_rebuilds;
    perf_ui_ctx.render_fill_success_last_frame = ctx.render_fill_success_last_frame->load(std::memory_order_relaxed);
    perf_ui_ctx.render_fill_attempts_last_frame = ctx.render_fill_attempts_last_frame->load(std::memory_order_relaxed);
    perf_ui_ctx.render_fill_no_triangles_last_frame = ctx.render_fill_no_triangles_last_frame->load(std::memory_order_relaxed);
    perf_ui_ctx.render_fill_bad_indices_last_frame = ctx.render_fill_bad_indices_last_frame->load(std::memory_order_relaxed);
    perf_ui_ctx.arkavo_room_id = ctx.arkavo_room_id;
    perf_ui_ctx.arkavo_room_id_size = ctx.arkavo_room_id_size;
    perf_ui_ctx.arkavo_connected = *ctx.arkavo_client != nullptr && (*ctx.arkavo_client)->isConnected();
    perf_ui_ctx.arkavo_self_peer_id = *ctx.arkavo_client ? (*ctx.arkavo_client)->selfPeerId() : std::string();
    perf_ui_ctx.arkavo_tracked_peers = *ctx.arkavo_client ? (*ctx.arkavo_client)->peers().size() : 0;
    perf_ui_ctx.arkavo_open_peers = std::move(arkavo_open_peers);
    perf_ui_ctx.arkavo_send_peer = ctx.arkavo_send_peer;
    perf_ui_ctx.arkavo_send_peer_size = ctx.arkavo_send_peer_size;
    perf_ui_ctx.arkavo_send_path = ctx.arkavo_send_path;
    perf_ui_ctx.arkavo_send_path_size = ctx.arkavo_send_path_size;
    perf_ui_ctx.arkavo_status = ctx.arkavo_status;
    perf_ui_ctx.arkavo_err = ctx.arkavo_err;
    perf_ui_ctx.on_connect_arkavo = [&]() { connect_arkavo(); };
    perf_ui_ctx.on_disconnect_arkavo = [&]() { disconnect_arkavo(); };
    perf_ui_ctx.on_send_arkavo_file = [&]() { send_arkavo_file(); };
    perf_ui_ctx.lan_panel = &lan_panel_ctx;
    perf_ui_ctx.tile_cache_size = g_TileCache.size();
    perf_ui_ctx.max_tile_cache = kMaxTileCache;
    perf_ui_ctx.cache_clear = &cache_clear_ui;
    perf_ui_ctx.on_clear_cache = [&]() { clear_cache_action(); };
    drawPerformanceStatsPanel(perf_ui_ctx);

    *ctx.clear_cache_all = cache_clear_ui.clear_cache_all;
    *ctx.clear_cache_hydration = cache_clear_ui.clear_cache_hydration;
    *ctx.clear_cache_triangulation = cache_clear_ui.clear_cache_triangulation;
    *ctx.clear_cache_derived = cache_clear_ui.clear_cache_derived;
    *ctx.clear_cache_heatmap_memory = cache_clear_ui.clear_cache_heatmap_memory;
    *ctx.clear_cache_heatmap_disk = cache_clear_ui.clear_cache_heatmap_disk;
    *ctx.clear_cache_tile_memory = cache_clear_ui.clear_cache_tile_memory;
    *ctx.clear_cache_tile_disk_presence = cache_clear_ui.clear_cache_tile_disk_presence;
    *ctx.last_cache_clear_msg = std::move(cache_clear_ui.last_cache_clear_msg);
}
