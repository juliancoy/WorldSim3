#pragma once

#include "dataset_library.h"
#include "lan_discovery.h"

#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class FreshnessState;
struct LayerDef;

enum class LayerDownloadItemState {
    Idle,
    Queued,
    Active,
    Succeeded,
    Failed,
};

struct LayerDownloadItemSnapshot {
    LayerDownloadItemState state = LayerDownloadItemState::Idle;
    float progress = 0.0f;
    double eta_seconds = -1.0;
    std::string status;
};

struct LayerDownloadTask {
    size_t idx = (size_t)-1;
    std::future<VersionedDownloadResult> future;
};

struct LayerDownloadEtaState {
    uint64_t last_bytes = 0;
    uint64_t total_bytes = 0;
    std::chrono::steady_clock::time_point last_sample_at{};
    double ewma_bps = 0.0;
};

struct LayerDownloadQueueContext {
    std::filesystem::path root;
    std::vector<LayerDef>* layers = nullptr;
    const std::vector<bool>* local_layer_exists_cache = nullptr;
    std::deque<size_t>* queue = nullptr;
    bool* inflight = nullptr;
    size_t* active_idx = nullptr;
    std::future<VersionedDownloadResult>* future = nullptr;
    std::string* active_file = nullptr;
    std::vector<LayerDownloadTask>* active_tasks = nullptr;
    size_t max_parallel_downloads = 4;
    std::mutex* item_state_mutex = nullptr;
    std::unordered_map<size_t, float>* item_progress = nullptr;
    std::unordered_map<size_t, std::chrono::steady_clock::time_point>* item_started_at = nullptr;
    std::unordered_map<size_t, LayerDownloadEtaState>* item_eta_state = nullptr;
    std::unordered_map<size_t, std::string>* item_status = nullptr;
    std::unordered_set<size_t>* item_failed = nullptr;
    std::string* last_event = nullptr;
    bool* queue_loaded = nullptr;
    std::string* data_library_status_msg = nullptr;
    std::vector<FreshnessState>* data_freshness_state = nullptr;
    std::vector<std::string>* data_freshness_msg = nullptr;
    LanDiscoveryContext* lan = nullptr;
    std::function<void(size_t, bool)> mark_local_layer_exists;
    std::function<void(size_t, bool)> enqueue_hydration;
};

std::filesystem::path layerDownloadQueuePath(const LayerDownloadQueueContext& ctx);
void persistLayerDownloadQueue(const LayerDownloadQueueContext& ctx);
bool layerDownloadPending(const LayerDownloadQueueContext& ctx, size_t idx);
void loadLayerDownloadQueue(LayerDownloadQueueContext& ctx);
bool enqueueLayerDownloadRequest(LayerDownloadQueueContext& ctx, size_t idx);
void startLayerDownload(LayerDownloadQueueContext& ctx, size_t idx);
void tickLayerDownloadQueue(LayerDownloadQueueContext& ctx);
LayerDownloadItemSnapshot layerDownloadItemSnapshot(const LayerDownloadQueueContext& ctx, size_t idx);
