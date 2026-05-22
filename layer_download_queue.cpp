#include "layer_download_queue.h"

#include "dataset_library.h"
#include "app_utils.h"
#include "layer_import.h"
#include "net_http_utils.h"
#include "types.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

std::filesystem::path layerDownloadQueuePath(const LayerDownloadQueueContext& ctx) {
    return ctx.root / "data" / "layer_download_queue.json";
}

void persistLayerDownloadQueue(const LayerDownloadQueueContext& ctx) {
    if (!ctx.layers || !ctx.queue || !ctx.inflight || !ctx.active_idx || !ctx.active_tasks) return;
    json j = json::object();
    j["schema_version"] = 1;
    j["queued"] = json::array();
    for (const auto& task : *ctx.active_tasks) {
        if (task.idx < ctx.layers->size()) j["queued"].push_back({{"file", (*ctx.layers)[task.idx].file}});
    }
    for (size_t idx : *ctx.queue) {
        if (idx < ctx.layers->size()) j["queued"].push_back({{"file", (*ctx.layers)[idx].file}});
    }
    fs::create_directories(ctx.root / "data");
    std::ofstream out(layerDownloadQueuePath(ctx));
    if (out) out << j.dump(2);
}

bool layerDownloadPending(const LayerDownloadQueueContext& ctx, size_t idx) {
    if (!ctx.layers || !ctx.queue || !ctx.inflight || !ctx.active_idx || !ctx.active_tasks) return true;
    if (idx >= ctx.layers->size()) return true;
    for (const auto& task : *ctx.active_tasks) if (task.idx == idx) return true;
    for (size_t q : *ctx.queue) if (q == idx) return true;
    return false;
}

void loadLayerDownloadQueue(LayerDownloadQueueContext& ctx) {
    if (!ctx.layers || !ctx.queue || !ctx.queue_loaded || !ctx.last_event || !ctx.data_library_status_msg) return;
    if (*ctx.queue_loaded) return;
    *ctx.queue_loaded = true;
    std::ifstream in(layerDownloadQueuePath(ctx));
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        *ctx.last_event = "Layer download queue file is invalid JSON; ignoring.";
        *ctx.data_library_status_msg = *ctx.last_event;
        return;
    }
    if (!j.contains("queued") || !j["queued"].is_array()) return;
    size_t loaded = 0;
    for (const auto& row : j["queued"]) {
        if (!row.is_object()) continue;
        const std::string file = row.value("file", std::string());
        if (file.empty()) continue;
        for (size_t idx = 0; idx < ctx.layers->size(); ++idx) {
            if ((*ctx.layers)[idx].file != file ||
                ((*ctx.layers)[idx].source_url.empty() && !layerHasImportSource((*ctx.layers)[idx])) ||
                layerDownloadPending(ctx, idx)) {
                continue;
            }
            ctx.queue->push_back(idx);
            loaded++;
            break;
        }
    }
    if (loaded > 0) {
        *ctx.last_event = "Loaded " + std::to_string(loaded) + " queued layer download(s) from previous run.";
        *ctx.data_library_status_msg = *ctx.last_event;
    }
}

bool enqueueLayerDownloadRequest(LayerDownloadQueueContext& ctx, size_t idx) {
    if (!ctx.layers || !ctx.queue || !ctx.inflight || !ctx.active_idx || !ctx.last_event || !ctx.data_library_status_msg) return false;
    if (idx >= ctx.layers->size()) return false;
    auto& layers = *ctx.layers;
    if (layers[idx].source_url.empty() && !layerHasImportSource(layers[idx])) {
        *ctx.last_event = "No source URL/import source for " + layers[idx].name;
        *ctx.data_library_status_msg = *ctx.last_event;
        return false;
    }
    if (*ctx.inflight && *ctx.active_idx == idx) {
        *ctx.last_event = "Already downloading: " + layers[idx].name;
        *ctx.data_library_status_msg = *ctx.last_event;
        return false;
    }
    for (size_t q : *ctx.queue) {
        if (q == idx) {
            *ctx.last_event = "Already queued: " + layers[idx].name;
            *ctx.data_library_status_msg = *ctx.last_event;
            return false;
        }
    }
    ctx.queue->push_back(idx);
    *ctx.last_event = "Queued layer download: " + layers[idx].name;
    *ctx.data_library_status_msg = *ctx.last_event;
    persistLayerDownloadQueue(ctx);
    return true;
}

void startLayerDownload(LayerDownloadQueueContext& ctx, size_t idx) {
    if (!ctx.layers || !ctx.inflight || !ctx.active_idx || !ctx.active_file || !ctx.active_tasks ||
        !ctx.last_event || !ctx.data_library_status_msg || !ctx.item_state_mutex || !ctx.item_progress ||
        !ctx.item_started_at || !ctx.item_eta_state || !ctx.item_status || !ctx.item_failed ||
        idx >= ctx.layers->size()) {
        return;
    }
    auto& layers = *ctx.layers;
    const LayerDef layer_for_download = layers[idx];
    const fs::path local_layer_path = provenanceStoredLayerPath(ctx.root, layers[idx]);
    const auto now = std::chrono::steady_clock::now();
    constexpr auto kLanAutoScanInterval = std::chrono::seconds(30);
    if (ctx.lan && ctx.lan->last_scan_at && now - *ctx.lan->last_scan_at > kLanAutoScanInterval) {
        (void)scanLanPeers(*ctx.lan, 250, false);
    }
    std::vector<std::pair<std::string, std::string>> peer_download_targets;
    if (ctx.lan && ctx.lan->peers) {
        for (const auto& peer : *ctx.lan->peers) {
            if (!peer.protocol_match || peer.ip.empty() || peer.dataset_port <= 0) continue;
            const std::string rel = provenanceStoredLayerPath(ctx.root, layers[idx]).lexically_relative(ctx.root).string();
            const std::string path_q = urlEncodeComponent(rel);
            std::string url = "http://" + peer.ip + ":" + std::to_string(peer.dataset_port) + "/dataset/file?path=" + path_q;
            std::string label = peer.ip + ":" + std::to_string(peer.dataset_port);
            peer_download_targets.push_back({std::move(label), std::move(url)});
        }
    }
    {
        std::lock_guard<std::mutex> lk(*ctx.item_state_mutex);
        (*ctx.item_status)[idx] = "Starting...";
        (*ctx.item_progress)[idx] = 0.0f;
        const auto t0 = std::chrono::steady_clock::now();
        (*ctx.item_started_at)[idx] = t0;
        (*ctx.item_eta_state)[idx] = LayerDownloadEtaState{0, 0, t0, 0.0};
        ctx.item_failed->erase(idx);
    }
    LayerDownloadTask task;
    task.idx = idx;
    task.future = std::async(std::launch::async, [root = ctx.root, layer_for_download, local_layer_path, peer_download_targets, idx, mtx = ctx.item_state_mutex, progress = ctx.item_progress, eta_state = ctx.item_eta_state, status = ctx.item_status]() {
        std::vector<std::string> peer_failures;
        for (const auto& candidate : peer_download_targets) {
            {
                std::lock_guard<std::mutex> lk(*mtx);
                (*status)[idx] = "Trying LAN peer " + candidate.first;
            }
            VersionedDownloadResult peer_res = downloadUrlVersioned(
                candidate.second,
                local_layer_path,
                root / "data" / "versions",
                [idx, mtx, progress, eta_state, status](uint64_t now, uint64_t total) {
                    std::lock_guard<std::mutex> lk(*mtx);
                    auto& eta = (*eta_state)[idx];
                    const auto t = std::chrono::steady_clock::now();
                    if (eta.last_sample_at.time_since_epoch().count() != 0 && now >= eta.last_bytes) {
                        const double dt = std::chrono::duration<double>(t - eta.last_sample_at).count();
                        const uint64_t dbytes = now - eta.last_bytes;
                        if (dt >= 0.20 && dbytes > 0) {
                            const double inst_bps = (double)dbytes / dt;
                            constexpr double kAlpha = 0.22;
                            eta.ewma_bps = eta.ewma_bps <= 0.0 ? inst_bps : (kAlpha * inst_bps + (1.0 - kAlpha) * eta.ewma_bps);
                        }
                    }
                    eta.last_sample_at = t;
                    eta.last_bytes = now;
                    eta.total_bytes = total;
                    if (total > 0) (*progress)[idx] = std::clamp((float)now / (float)total, 0.0f, 0.99f);
                    (*status)[idx] = total > 0
                        ? ("Downloading " + std::to_string(now / 1024) + "KB / " + std::to_string(total / 1024) + "KB")
                        : ("Downloading " + std::to_string(now / 1024) + "KB");
                });
            if (peer_res.ok) {
                peer_res.message = "LAN peer " + candidate.first + " | " + peer_res.message;
                return peer_res;
            }
            peer_failures.push_back(candidate.first + ": " + peer_res.message);
        }
        {
            std::lock_guard<std::mutex> lk(*mtx);
            (*status)[idx] = "Trying source";
        }
        VersionedDownloadResult src_res = downloadOrImportLayer(layer_for_download, local_layer_path, root);
        if (!peer_failures.empty()) {
            if (src_res.ok) {
                src_res.message = "source fallback after LAN peer failures | " + src_res.message;
            } else {
                src_res.message = "LAN peers and source failed | " + src_res.message;
            }
        }
        return src_res;
    });
    ctx.active_tasks->push_back(std::move(task));
    *ctx.inflight = true;
    if (*ctx.active_idx == (size_t)-1) {
        *ctx.active_idx = idx;
        *ctx.active_file = layers[idx].file;
    }
    *ctx.last_event = "Started layer download: " + layers[idx].name;
    *ctx.data_library_status_msg = *ctx.last_event;
    persistLayerDownloadQueue(ctx);
}

void tickLayerDownloadQueue(LayerDownloadQueueContext& ctx) {
    if (!ctx.layers || !ctx.queue || !ctx.inflight || !ctx.active_idx || !ctx.active_file ||
        !ctx.active_tasks || !ctx.last_event || !ctx.data_library_status_msg || !ctx.data_freshness_state ||
        !ctx.data_freshness_msg || !ctx.item_state_mutex || !ctx.item_progress || !ctx.item_started_at ||
        !ctx.item_eta_state || !ctx.item_status || !ctx.item_failed) {
        return;
    }
    const size_t max_parallel = std::max<size_t>(1, ctx.max_parallel_downloads);
    while (ctx.active_tasks->size() < max_parallel && !ctx.queue->empty()) {
        const size_t idx = ctx.queue->front();
        ctx.queue->pop_front();
        persistLayerDownloadQueue(ctx);
        startLayerDownload(ctx, idx);
    }
    for (size_t i = 0; i < ctx.active_tasks->size();) {
        auto& task = (*ctx.active_tasks)[i];
        if (!task.future.valid() || task.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++i;
            continue;
        }
        VersionedDownloadResult vd = task.future.get();
        const size_t idx = task.idx;
        if (idx < ctx.layers->size()) {
            auto& layers = *ctx.layers;
            if (vd.ok) {
                *ctx.data_library_status_msg =
                    (vd.not_modified ? "Checked " : "Downloaded/updated ") + layers[idx].file + " (" + vd.message + ")";
                if (idx < ctx.data_freshness_state->size()) (*ctx.data_freshness_state)[idx] = FreshnessState::UpToDate;
                if (idx < ctx.data_freshness_msg->size()) (*ctx.data_freshness_msg)[idx] = vd.message;
                if (ctx.mark_local_layer_exists) ctx.mark_local_layer_exists(idx, true);
                if (ctx.enqueue_hydration) ctx.enqueue_hydration(idx, true);
                *ctx.last_event = "Completed: " + layers[idx].name;
                {
                    std::lock_guard<std::mutex> lk(*ctx.item_state_mutex);
                    (*ctx.item_progress)[idx] = 1.0f;
                    (*ctx.item_status)[idx] = vd.message;
                    ctx.item_started_at->erase(idx);
                    ctx.item_eta_state->erase(idx);
                    ctx.item_failed->erase(idx);
                }
            } else {
                *ctx.data_library_status_msg = "Download failed for " + layers[idx].file + ": " + vd.message;
                if (idx < ctx.data_freshness_state->size()) (*ctx.data_freshness_state)[idx] = FreshnessState::Error;
                if (idx < ctx.data_freshness_msg->size()) (*ctx.data_freshness_msg)[idx] = vd.message;
                *ctx.last_event = "Failed: " + layers[idx].name + " (" + vd.message + ")";
                {
                    std::lock_guard<std::mutex> lk(*ctx.item_state_mutex);
                    (*ctx.item_status)[idx] = vd.message;
                    ctx.item_started_at->erase(idx);
                    ctx.item_eta_state->erase(idx);
                    ctx.item_failed->insert(idx);
                }
            }
            persistLayerDownloadQueue(ctx);
        }
        ctx.active_tasks->erase(ctx.active_tasks->begin() + (std::ptrdiff_t)i);
    }
    *ctx.inflight = !ctx.active_tasks->empty();
    if (!ctx.active_tasks->empty()) {
        *ctx.active_idx = (*ctx.active_tasks)[0].idx;
        if (*ctx.active_idx < ctx.layers->size()) *ctx.active_file = (*ctx.layers)[*ctx.active_idx].file;
    } else {
        *ctx.active_idx = (size_t)-1;
        ctx.active_file->clear();
    }
}

LayerDownloadItemSnapshot layerDownloadItemSnapshot(const LayerDownloadQueueContext& ctx, size_t idx) {
    LayerDownloadItemSnapshot out;
    if (!ctx.layers || idx >= ctx.layers->size()) return out;
    if (ctx.local_layer_exists_cache && idx < ctx.local_layer_exists_cache->size() && (*ctx.local_layer_exists_cache)[idx]) {
        out.state = LayerDownloadItemState::Succeeded;
        out.progress = 1.0f;
        out.status = "Available locally";
    }
    if (ctx.item_state_mutex && ctx.item_failed && ctx.item_progress && ctx.item_started_at &&
        ctx.item_eta_state && ctx.item_status) {
        std::lock_guard<std::mutex> lk(*ctx.item_state_mutex);
        auto pit = ctx.item_progress->find(idx);
        auto eit = ctx.item_eta_state->find(idx);
        auto sit = ctx.item_status->find(idx);
        if (pit != ctx.item_progress->end()) out.progress = pit->second;
        if (eit != ctx.item_eta_state->end()) {
            const auto& eta = eit->second;
            if (eta.total_bytes > 0 && eta.last_bytes < eta.total_bytes && eta.ewma_bps > 1024.0) {
                const uint64_t remaining = eta.total_bytes - eta.last_bytes;
                const double eta_s = (double)remaining / eta.ewma_bps;
                if (eta_s >= 0.0 && eta_s < 24.0 * 3600.0) out.eta_seconds = eta_s;
            }
        }
        if (sit != ctx.item_status->end()) out.status = sit->second;
        if (ctx.item_failed->find(idx) != ctx.item_failed->end()) out.state = LayerDownloadItemState::Failed;
    }
    if (out.state != LayerDownloadItemState::Failed && layerDownloadPending(ctx, idx)) {
        bool active = false;
        if (ctx.active_tasks) {
            for (const auto& task : *ctx.active_tasks) {
                if (task.idx == idx) {
                    active = true;
                    break;
                }
            }
        }
        out.state = active ? LayerDownloadItemState::Active : LayerDownloadItemState::Queued;
        if (active && out.progress < 0.01f) out.progress = 0.01f;
    }
    return out;
}
