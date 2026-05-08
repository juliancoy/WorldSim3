#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <utility>
#include <functional>

struct DownloadQueueMetrics {
    bool inflight = false;
    size_t done = 0;
    size_t total = 0;
    size_t missing_total = 0;
    size_t downloaded = 0;
    size_t skipped = 0;
    size_t failed = 0;
    size_t queued = 0;
    double tiles_per_s = 0.0;
    double mb_per_s = 0.0;
    double eta_s = 0.0;
    double bytes_mb = 0.0;
    float progress = 0.0f;
    std::string label;
    std::string progress_line;
};

struct DownloadQueueState {
    bool inflight = false;
    std::future<void> worker_future;
    std::mutex mutex;
    std::string progress;
    std::string label;
    std::string active_url_tmpl;
    std::deque<std::pair<std::string, std::string>> queue;
    std::chrono::steady_clock::time_point started_at{};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> stopped{false};
    std::atomic<size_t> done{0};
    std::atomic<size_t> total{0};
    std::atomic<size_t> missing_total{0};
    std::atomic<size_t> downloaded{0};
    std::atomic<size_t> skipped{0};
    std::atomic<size_t> failed{0};
    std::atomic<size_t> bytes{0};
    bool loaded_from_disk = false;
    std::string last_queue_event;
};

void requestBasemapDownload(
    DownloadQueueState& state,
    const std::filesystem::path& root,
    const std::string& target_dir,
    const std::string& url_tmpl,
    std::string& status_msg);

void tickBasemapDownloadQueue(DownloadQueueState& state, const std::filesystem::path& root, std::string& status_msg);
void stopBasemapDownloads(DownloadQueueState& state, const std::filesystem::path& root, std::string& status_msg);
DownloadQueueMetrics snapshotDownloadQueueMetrics(DownloadQueueState& state);
void drawBasemapDownloadQueueSection(
    DownloadQueueState& state,
    const std::filesystem::path& root,
    std::string& status_msg,
    const std::function<std::string(const std::string&)>& resolve_label = {});
void loadBasemapDownloadQueue(DownloadQueueState& state, const std::filesystem::path& root, std::string& status_msg);
void persistBasemapDownloadQueue(DownloadQueueState& state, const std::filesystem::path& root);
