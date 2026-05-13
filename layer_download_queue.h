#pragma once

#include "dataset_library.h"
#include "lan_discovery.h"

#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <vector>

enum class FreshnessState;
struct LayerDef;

struct LayerDownloadQueueContext {
    std::filesystem::path root;
    std::vector<LayerDef>* layers = nullptr;
    std::deque<size_t>* queue = nullptr;
    bool* inflight = nullptr;
    size_t* active_idx = nullptr;
    std::future<VersionedDownloadResult>* future = nullptr;
    std::string* active_file = nullptr;
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
