#pragma once

#include "dataset_library.h"
#include "layer_registry.h"
#include "types.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct DataLibraryCoordinatorContext {
    std::filesystem::path root;
    std::vector<LayerDef>* layers = nullptr;
    LayerRegistry* layer_registry = nullptr;
    std::vector<bool>* local_layer_exists_cache = nullptr;
    std::vector<FreshnessState>* data_freshness_state = nullptr;
    std::vector<std::string>* data_freshness_msg = nullptr;
    std::string* data_library_status_msg = nullptr;
    bool* data_library_bulk_inflight = nullptr;
    std::mutex* data_library_bulk_mutex = nullptr;
    std::string* data_library_bulk_progress = nullptr;
    std::future<LayerDownloadSummary>* data_library_bulk_future = nullptr;
    std::function<void()> refresh_local_layer_exists_cache;
    std::function<void(size_t, bool)> enqueue_hydration;
    std::function<bool(size_t)> enqueue_layer_download_request;
    std::function<bool(size_t)> layer_download_pending;
    std::string* layer_download_last_event = nullptr;
};

std::string resolveDataLibraryDownloadLabel(
    const LayerRegistry& layer_registry,
    const std::vector<LayerDef>& layers,
    const std::string& key);
bool finalizeDataLibraryBulkDownloadIfReady(DataLibraryCoordinatorContext& ctx);
void checkAllDataLibraryUpdates(DataLibraryCoordinatorContext& ctx);
void rescanDataLibraryLocalFiles(DataLibraryCoordinatorContext& ctx);
void startDataLibraryPhaseDownload(DataLibraryCoordinatorContext& ctx, const std::string& phase, bool include_large);
size_t queueAllMissingLayerDownloads(DataLibraryCoordinatorContext& ctx);
