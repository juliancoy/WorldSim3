#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <string>
#include <unordered_set>

struct LazyTileDownloadRequest {
    std::string target_dir;
    std::string url_tmpl;
    int z = 0;
    int x = 0;
    int y = 0;
};

struct LazyTileDownloadResult {
    LazyTileDownloadRequest request;
    bool ok = false;
    bool skipped = false;
    size_t bytes = 0;
    std::string error;
};

struct LazyTileDownloadState {
    bool inflight = false;
    bool loaded_from_disk = false;
    std::future<LazyTileDownloadResult> worker_future;
    std::deque<LazyTileDownloadRequest> queue;
    std::unordered_set<std::string> queued_keys;
    std::unordered_set<std::string> failed_keys;
    std::chrono::steady_clock::time_point started_at{};
    std::atomic<size_t> downloaded{0};
    std::atomic<size_t> skipped{0};
    std::atomic<size_t> failed{0};
    std::atomic<size_t> bytes{0};
    std::string progress;
};

void loadLazyTileDownloadQueue(LazyTileDownloadState& state, const std::filesystem::path& root, std::string& status_msg);
void persistLazyTileDownloadQueue(LazyTileDownloadState& state, const std::filesystem::path& root);
void requestLazyBasemapTile(
    LazyTileDownloadState& state,
    const std::filesystem::path& root,
    const std::string& target_dir,
    const std::string& url_tmpl,
    int z,
    int x,
    int y);
void tickLazyTileDownloadQueue(LazyTileDownloadState& state, const std::filesystem::path& root, std::string& status_msg);
void drawLazyTileDownloadQueueSection(LazyTileDownloadState& state);
