#include "layer_download_queue.h"

#include "dataset_library.h"
#include "layer_import.h"
#include "net_http_utils.h"
#include "types.h"

#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

std::filesystem::path layerDownloadQueuePath(const LayerDownloadQueueContext& ctx) {
    return ctx.root / "data" / "layer_download_queue.json";
}

void persistLayerDownloadQueue(const LayerDownloadQueueContext& ctx) {
    if (!ctx.layers || !ctx.queue || !ctx.inflight || !ctx.active_idx) return;
    json j = json::object();
    j["schema_version"] = 1;
    j["queued"] = json::array();
    if (*ctx.inflight && *ctx.active_idx < ctx.layers->size()) {
        j["queued"].push_back({{"file", (*ctx.layers)[*ctx.active_idx].file}});
    }
    for (size_t idx : *ctx.queue) {
        if (idx < ctx.layers->size()) j["queued"].push_back({{"file", (*ctx.layers)[idx].file}});
    }
    fs::create_directories(ctx.root / "data");
    std::ofstream out(layerDownloadQueuePath(ctx));
    if (out) out << j.dump(2);
}

bool layerDownloadPending(const LayerDownloadQueueContext& ctx, size_t idx) {
    if (!ctx.layers || !ctx.queue || !ctx.inflight || !ctx.active_idx) return true;
    if (idx >= ctx.layers->size()) return true;
    if (*ctx.inflight && *ctx.active_idx == idx) return true;
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
    if (!ctx.layers || !ctx.inflight || !ctx.active_idx || !ctx.future || !ctx.active_file ||
        !ctx.last_event || !ctx.data_library_status_msg || idx >= ctx.layers->size()) {
        return;
    }
    auto& layers = *ctx.layers;
    *ctx.inflight = true;
    *ctx.active_idx = idx;
    *ctx.active_file = layers[idx].file;
    const LayerDef layer_for_download = layers[idx];
    const fs::path local_layer_path = ctx.root / "data" / "layers" / layers[idx].file;
    const auto now = std::chrono::steady_clock::now();
    constexpr auto kLanAutoScanInterval = std::chrono::seconds(30);
    if (ctx.lan && ctx.lan->last_scan_at && now - *ctx.lan->last_scan_at > kLanAutoScanInterval) {
        (void)scanLanPeers(*ctx.lan, 250, false);
    }
    std::vector<std::pair<std::string, std::string>> peer_download_targets;
    if (ctx.lan && ctx.lan->peers) {
        for (const auto& peer : *ctx.lan->peers) {
            if (!peer.protocol_match || peer.ip.empty() || peer.dataset_port <= 0) continue;
            const std::string rel = "data/layers/" + layers[idx].file;
            const std::string path_q = urlEncodeComponent(rel);
            std::string url = "http://" + peer.ip + ":" + std::to_string(peer.dataset_port) + "/dataset/file?path=" + path_q;
            std::string label = peer.ip + ":" + std::to_string(peer.dataset_port);
            peer_download_targets.push_back({std::move(label), std::move(url)});
        }
    }
    ctx.future->operator=(std::async(std::launch::async, [root = ctx.root, layer_for_download, local_layer_path, peer_download_targets]() {
        std::vector<std::string> peer_failures;
        for (const auto& candidate : peer_download_targets) {
            VersionedDownloadResult peer_res = downloadUrlVersioned(candidate.second, local_layer_path, root / "data" / "versions");
            if (peer_res.ok) {
                peer_res.message = "LAN peer " + candidate.first + " | " + peer_res.message;
                return peer_res;
            }
            peer_failures.push_back(candidate.first + ": " + peer_res.message);
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
    }));
    *ctx.last_event = "Started layer download: " + layers[idx].name;
    *ctx.data_library_status_msg = *ctx.last_event;
    persistLayerDownloadQueue(ctx);
}

void tickLayerDownloadQueue(LayerDownloadQueueContext& ctx) {
    if (!ctx.layers || !ctx.queue || !ctx.inflight || !ctx.active_idx || !ctx.future ||
        !ctx.last_event || !ctx.data_library_status_msg || !ctx.data_freshness_state ||
        !ctx.data_freshness_msg) {
        return;
    }
    if (!*ctx.inflight && !ctx.queue->empty()) {
        const size_t idx = ctx.queue->front();
        ctx.queue->pop_front();
        persistLayerDownloadQueue(ctx);
        startLayerDownload(ctx, idx);
    }
    if (*ctx.inflight && ctx.future->valid() &&
        ctx.future->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        VersionedDownloadResult vd = ctx.future->get();
        const size_t idx = *ctx.active_idx;
        *ctx.inflight = false;
        *ctx.active_idx = (size_t)-1;
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
            } else {
                *ctx.data_library_status_msg = "Download failed for " + layers[idx].file + ": " + vd.message;
                if (idx < ctx.data_freshness_state->size()) (*ctx.data_freshness_state)[idx] = FreshnessState::Error;
                if (idx < ctx.data_freshness_msg->size()) (*ctx.data_freshness_msg)[idx] = vd.message;
                *ctx.last_event = "Failed: " + layers[idx].name + " (" + vd.message + ")";
            }
            persistLayerDownloadQueue(ctx);
        }
    }
}
