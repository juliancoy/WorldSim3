#include "frame_prelude.h"

FramePreludeResult runFramePrelude(const FramePreludeContext& ctx) {
    FramePreludeResult result;
    if (!ctx.root || !ctx.layers || !ctx.layer_profile_accumulators || !ctx.layer_profile_dirty ||
        !ctx.layer_profile_snapshot || !ctx.layer_profile_mutex || !ctx.layer_registry ||
        !ctx.local_layer_exists_cache || !ctx.data_freshness_state || !ctx.data_freshness_msg ||
        !ctx.data_library_status_msg || !ctx.layer_download_queue || !ctx.layer_download_inflight ||
        !ctx.layer_download_active_idx || !ctx.layer_download_future || !ctx.layer_download_active_file ||
        !ctx.layer_download_last_event || !ctx.layer_download_queue_loaded || !ctx.lan_peers ||
        !ctx.lan_scan_status || !ctx.lan_last_scan_at || !ctx.center_lon || !ctx.center_lat ||
        !ctx.zoom || !ctx.current_zoom_state || !ctx.current_lon_state || !ctx.current_lat_state ||
        !ctx.api_layer_mutex || !ctx.api_layer_enable_cmds || !ctx.api_layer_fill_cmds ||
        !ctx.api_layer_download_cmds || !ctx.layer_fill_mutex || !ctx.layer_fill_enabled ||
        !ctx.layer_fill_state_changed || !ctx.api_ui_cmd_seq || !ctx.api_ui_cmd_kind ||
        !ctx.api_ui_cmd_x || !ctx.api_ui_cmd_y || !ctx.api_ui_cmd_button ||
        !ctx.api_ui_cmd_scroll_y || !ctx.api_ui_cmd_last_seq || !ctx.api_ui_mouse_release_pending ||
        !ctx.api_ui_mouse_release_button || !ctx.api_zoom_cmd || !ctx.api_lon_cmd ||
        !ctx.api_lat_cmd || !ctx.lazy_tile_download || !ctx.basemap_coverage_dirty ||
        !ctx.basemap_availability_last_check || !ctx.osm_missing_tiles_cached ||
        !ctx.osm_total_tiles_cached || !ctx.topo_missing_tiles_cached ||
        !ctx.topo_total_tiles_cached || !ctx.basemap_source_has_any_files_cached ||
        !ctx.topo_tiles_available_cached || !ctx.topo_vector_available_cached) {
        return result;
    }

    result.layer_profile_snapshot.layers = ctx.layers;
    result.layer_profile_snapshot.layer_profile_accumulators = ctx.layer_profile_accumulators;
    result.layer_profile_snapshot.layer_profile_dirty = ctx.layer_profile_dirty;
    result.layer_profile_snapshot.layer_profile_snapshot = ctx.layer_profile_snapshot;
    result.layer_profile_snapshot.layer_profile_mutex = ctx.layer_profile_mutex;

    result.lan_discovery.peers = ctx.lan_peers;
    result.lan_discovery.scan_status = ctx.lan_scan_status;
    result.lan_discovery.last_scan_at = ctx.lan_last_scan_at;
    result.lan_discovery.protocol_version = ctx.protocol_version;

    result.layer_download.root = *ctx.root;
    result.layer_download.layers = ctx.layers;
    result.layer_download.queue = ctx.layer_download_queue;
    result.layer_download.inflight = ctx.layer_download_inflight;
    result.layer_download.active_idx = ctx.layer_download_active_idx;
    result.layer_download.future = ctx.layer_download_future;
    result.layer_download.active_file = ctx.layer_download_active_file;
    result.layer_download.last_event = ctx.layer_download_last_event;
    result.layer_download.queue_loaded = ctx.layer_download_queue_loaded;
    result.layer_download.data_library_status_msg = ctx.data_library_status_msg;
    result.layer_download.data_freshness_state = ctx.data_freshness_state;
    result.layer_download.data_freshness_msg = ctx.data_freshness_msg;
    result.layer_download.lan = &result.lan_discovery;
    result.layer_download.mark_local_layer_exists = ctx.mark_local_layer_exists;
    result.layer_download.enqueue_hydration = ctx.enqueue_hydration;

    result.queue_all_missing_layer_downloads = [&ctx, &result]() {
        DataLibraryCoordinatorContext data_library_ctx;
        data_library_ctx.root = *ctx.root;
        data_library_ctx.layers = ctx.layers;
        data_library_ctx.layer_registry = ctx.layer_registry;
        data_library_ctx.local_layer_exists_cache = ctx.local_layer_exists_cache;
        data_library_ctx.data_freshness_state = ctx.data_freshness_state;
        data_library_ctx.data_freshness_msg = ctx.data_freshness_msg;
        data_library_ctx.data_library_status_msg = ctx.data_library_status_msg;
        data_library_ctx.refresh_local_layer_exists_cache = ctx.refresh_local_layer_exists_cache;
        data_library_ctx.enqueue_layer_download_request = [&](size_t idx) {
            return enqueueLayerDownloadRequest(result.layer_download, idx);
        };
        data_library_ctx.layer_download_pending = [&](size_t idx) {
            return layerDownloadPending(result.layer_download, idx);
        };
        data_library_ctx.layer_download_last_event = ctx.layer_download_last_event;
        return queueAllMissingLayerDownloads(data_library_ctx);
    };

    LayerApiCommandCoordinatorContext layer_api_ctx;
    layer_api_ctx.layers = ctx.layers;
    layer_api_ctx.layer_registry = ctx.layer_registry;
    layer_api_ctx.api_layer_mutex = ctx.api_layer_mutex;
    layer_api_ctx.api_layer_enable_cmds = ctx.api_layer_enable_cmds;
    layer_api_ctx.api_layer_fill_cmds = ctx.api_layer_fill_cmds;
    layer_api_ctx.api_layer_download_cmds = ctx.api_layer_download_cmds;
    layer_api_ctx.layer_profile_dirty = ctx.layer_profile_dirty;
    layer_api_ctx.layer_fill_mutex = ctx.layer_fill_mutex;
    layer_api_ctx.layer_fill_enabled = ctx.layer_fill_enabled;
    layer_api_ctx.layer_fill_state_changed = ctx.layer_fill_state_changed;
    layer_api_ctx.data_library_status_msg = ctx.data_library_status_msg;
    layer_api_ctx.enqueue_layer_download_request = [&](size_t idx) {
        return enqueueLayerDownloadRequest(result.layer_download, idx);
    };

    ApiControlContext api_control_ctx;
    api_control_ctx.zoom = ctx.zoom;
    api_control_ctx.center_lon = ctx.center_lon;
    api_control_ctx.center_lat = ctx.center_lat;
    api_control_ctx.api_zoom_cmd = ctx.api_zoom_cmd;
    api_control_ctx.api_lon_cmd = ctx.api_lon_cmd;
    api_control_ctx.api_lat_cmd = ctx.api_lat_cmd;
    api_control_ctx.min_zoom = ctx.min_zoom;
    api_control_ctx.max_zoom = ctx.max_zoom;
    api_control_ctx.layer_api = &layer_api_ctx;
    api_control_ctx.api_ui_cmd_seq = ctx.api_ui_cmd_seq;
    api_control_ctx.api_ui_cmd_kind = ctx.api_ui_cmd_kind;
    api_control_ctx.api_ui_cmd_x = ctx.api_ui_cmd_x;
    api_control_ctx.api_ui_cmd_y = ctx.api_ui_cmd_y;
    api_control_ctx.api_ui_cmd_button = ctx.api_ui_cmd_button;
    api_control_ctx.api_ui_cmd_scroll_y = ctx.api_ui_cmd_scroll_y;
    api_control_ctx.map_filter_state = ctx.map_filter_state;
    api_control_ctx.active_filter_result_set = ctx.active_filter_result_set;
    api_control_ctx.query_layers = ctx.query_layers;
    api_control_ctx.active_filter_status = ctx.active_filter_status;
    api_control_ctx.api_control_mutex = ctx.api_control_mutex;
    api_control_ctx.api_filter_control_cmd = ctx.api_filter_control_cmd;
    api_control_ctx.api_query_control_cmds = ctx.api_query_control_cmds;
    api_control_ctx.filter_state_key = ctx.filter_state_key;
    api_control_ctx.api_ui_cmd_last_seq = ctx.api_ui_cmd_last_seq;
    api_control_ctx.api_ui_mouse_release_pending = ctx.api_ui_mouse_release_pending;
    api_control_ctx.api_ui_mouse_release_button = ctx.api_ui_mouse_release_button;
    applyApiControlCommands(api_control_ctx);

    ctx.current_zoom_state->store(*ctx.zoom, std::memory_order_relaxed);
    ctx.current_lon_state->store(*ctx.center_lon, std::memory_order_relaxed);
    ctx.current_lat_state->store(*ctx.center_lat, std::memory_order_relaxed);

    loadLazyTileDownloadQueue(*ctx.lazy_tile_download, *ctx.root, *ctx.data_library_status_msg);
    loadLayerDownloadQueue(result.layer_download);
    tickLazyTileDownloadQueue(*ctx.lazy_tile_download, *ctx.root, *ctx.data_library_status_msg);

    const auto basemap_now = std::chrono::steady_clock::now();
    if (*ctx.basemap_coverage_dirty ||
        ctx.basemap_availability_last_check->time_since_epoch().count() == 0 ||
        (basemap_now - *ctx.basemap_availability_last_check) >= ctx.basemap_coverage_refresh_interval) {
        const BasemapCoverage osm_coverage = countBasemapCoverage(*ctx.root, "tiles", ctx.min_tile_zoom, ctx.max_native_tile_zoom);
        const BasemapCoverage topo_coverage = countBasemapCoverage(*ctx.root, "tiles_topo", ctx.min_tile_zoom, ctx.max_native_tile_zoom);
        *ctx.osm_missing_tiles_cached = osm_coverage.missing;
        *ctx.osm_total_tiles_cached = osm_coverage.total;
        *ctx.topo_missing_tiles_cached = topo_coverage.missing;
        *ctx.topo_total_tiles_cached = topo_coverage.total;
        *ctx.basemap_source_has_any_files_cached = true;
        *ctx.topo_tiles_available_cached = true;
        *ctx.topo_vector_available_cached = std::filesystem::exists(*ctx.root / "data" / "tiles_topo_vector.geojson");
        *ctx.basemap_availability_last_check = basemap_now;
        *ctx.basemap_coverage_dirty = false;
    }

    tickLayerDownloadQueue(result.layer_download);
    return result;
}
