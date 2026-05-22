#include "data_library_coordinator.h"

#include "app_utils.h"
#include "layer_import.h"
#include "thread_utils.h"

#include <future>

namespace fs = std::filesystem;

std::string resolveDataLibraryDownloadLabel(
    const LayerRegistry& layer_registry,
    const std::vector<LayerDef>& layers,
    const std::string& key) {
    if (key == "tiles") return "OpenStreetMap";
    if (key == "tiles_topo" || key == "tiles_topographic") return "Topographic";
    if (key == "tiles_satellite") return "Satellite";
    const int idx = layer_registry.findLayerByFileOrName(key);
    return (idx >= 0 && (size_t)idx < layers.size()) ? layers[(size_t)idx].name : key;
}

bool finalizeDataLibraryBulkDownloadIfReady(DataLibraryCoordinatorContext& ctx) {
    if (!ctx.data_library_bulk_inflight || !ctx.data_library_bulk_future || !*ctx.data_library_bulk_inflight) return false;
    if (!ctx.data_library_bulk_future->valid() ||
        ctx.data_library_bulk_future->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return false;
    }

    LayerDownloadSummary summary = ctx.data_library_bulk_future->get();
    *ctx.data_library_bulk_inflight = false;
    if (ctx.refresh_local_layer_exists_cache) ctx.refresh_local_layer_exists_cache();
    if (ctx.data_library_status_msg) {
        *ctx.data_library_status_msg = "Downloaded phase: " + std::to_string(summary.downloaded) +
            " fetched/checked, " + std::to_string(summary.skipped) +
            " skipped, " + std::to_string(summary.failed) + " failed";
    }
    if (ctx.data_library_bulk_mutex && ctx.data_library_bulk_progress) {
        std::lock_guard<std::mutex> lk(*ctx.data_library_bulk_mutex);
        ctx.data_library_bulk_progress->clear();
    }
    if (!ctx.layers || !ctx.local_layer_exists_cache || !ctx.data_freshness_state || !ctx.data_freshness_msg) return true;
    for (size_t i = 0; i < ctx.layers->size(); ++i) {
        if (i < ctx.local_layer_exists_cache->size() && (*ctx.local_layer_exists_cache)[i]) {
            (*ctx.data_freshness_state)[i] = FreshnessState::UpToDate;
            if ((*ctx.data_freshness_msg)[i].empty() || (*ctx.data_freshness_msg)[i] == "not downloaded") {
                (*ctx.data_freshness_msg)[i] = "downloaded";
            }
            if (ctx.enqueue_hydration) ctx.enqueue_hydration(i, true);
        }
    }
    return true;
}

void checkAllDataLibraryUpdates(DataLibraryCoordinatorContext& ctx) {
    if (!ctx.layers || !ctx.local_layer_exists_cache || !ctx.data_freshness_state || !ctx.data_freshness_msg) return;
    size_t checked = 0;
    size_t updates = 0;
    for (size_t i = 0; i < ctx.layers->size(); ++i) {
        const auto& l = (*ctx.layers)[i];
        const bool local_exists = i < ctx.local_layer_exists_cache->size() ? (*ctx.local_layer_exists_cache)[i] : false;
        const fs::path local_path = provenanceStoredLayerPath(ctx.root, l);
        const std::string freshness_url = l.source_url.empty() ? l.import_url : l.source_url;
        if (!local_exists || freshness_url.empty()) {
            (*ctx.data_freshness_state)[i] = freshness_url.empty() ? FreshnessState::NotTrackable : FreshnessState::Unknown;
            (*ctx.data_freshness_msg)[i] = freshness_url.empty() ? "no source URL/import source" : "not downloaded";
            continue;
        }
        FreshnessCheckResult cr = checkUrlFreshnessVersioned(freshness_url, local_path, ctx.root / "data" / "versions");
        (*ctx.data_freshness_state)[i] = cr.state;
        (*ctx.data_freshness_msg)[i] = cr.message;
        checked++;
        if (cr.state == FreshnessState::UpdateAvailable) updates++;
    }
    if (ctx.data_library_status_msg) {
        *ctx.data_library_status_msg = "Checked " + std::to_string(checked) + " datasets; updates available: " + std::to_string(updates);
    }
}

void rescanDataLibraryLocalFiles(DataLibraryCoordinatorContext& ctx) {
    if (ctx.refresh_local_layer_exists_cache) ctx.refresh_local_layer_exists_cache();
    if (ctx.data_library_status_msg) *ctx.data_library_status_msg = "Rescanned local layer files";
}

void startDataLibraryPhaseDownload(DataLibraryCoordinatorContext& ctx, const std::string& phase, bool include_large) {
    if (!ctx.data_library_bulk_inflight || !ctx.data_library_bulk_future || !ctx.data_library_bulk_mutex || !ctx.data_library_bulk_progress) return;
    {
        std::lock_guard<std::mutex> lk(*ctx.data_library_bulk_mutex);
        *ctx.data_library_bulk_progress = "Starting " + phase + " download";
    }
    if (ctx.data_library_status_msg) *ctx.data_library_status_msg = "Downloading " + phase + " in background";
    *ctx.data_library_bulk_inflight = true;
    const fs::path root = ctx.root;
    std::mutex* progress_mutex = ctx.data_library_bulk_mutex;
    std::string* progress = ctx.data_library_bulk_progress;
    *ctx.data_library_bulk_future = std::async(std::launch::async, [root, phase, include_large, progress_mutex, progress]() {
        setCurrentThreadName("ws3-data-fetch");
        return downloadLayerManifestPhase(
            root,
            phase,
            include_large,
            [progress_mutex, progress](size_t i, size_t total, const std::string& msg) {
                std::lock_guard<std::mutex> lk(*progress_mutex);
                *progress = "[" + std::to_string(i) + "/" + std::to_string(total) + "] " + msg;
            });
    });
}

size_t queueAllMissingLayerDownloads(DataLibraryCoordinatorContext& ctx) {
    if (!ctx.layers || !ctx.local_layer_exists_cache || !ctx.data_library_status_msg ||
        !ctx.enqueue_layer_download_request || !ctx.layer_download_pending) {
        return 0;
    }
    if (ctx.refresh_local_layer_exists_cache) ctx.refresh_local_layer_exists_cache();
    size_t queued = 0;
    size_t missing_without_source = 0;
    size_t already_present = 0;
    size_t already_pending = 0;
    for (size_t idx = 0; idx < ctx.layers->size(); ++idx) {
        const bool local_exists = idx < ctx.local_layer_exists_cache->size() ? (*ctx.local_layer_exists_cache)[idx] : false;
        if (local_exists) {
            already_present++;
            continue;
        }
        if ((*ctx.layers)[idx].source_url.empty() && !layerHasImportSource((*ctx.layers)[idx])) {
            missing_without_source++;
            continue;
        }
        if (ctx.layer_download_pending(idx)) {
            already_pending++;
            continue;
        }
        if (ctx.enqueue_layer_download_request(idx)) queued++;
    }
    *ctx.data_library_status_msg =
        "Download All queued " + std::to_string(queued) +
        " missing layer(s); " + std::to_string(already_present) +
        " already present, " + std::to_string(already_pending) +
        " already queued/active, " + std::to_string(missing_without_source) +
        " missing without source URL";
    if (ctx.layer_download_last_event) *ctx.layer_download_last_event = *ctx.data_library_status_msg;
    return queued;
}
