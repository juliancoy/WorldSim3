#include "download_queue.h"

#include "app_utils.h"
#include "dataset_library.h"

#include "imgui.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

static fs::path queueFilePath(const fs::path& root) {
    return root / "data" / "download_queue.json";
}

void persistBasemapDownloadQueue(DownloadQueueState& state, const fs::path& root) {
    json j = json::object();
    j["schema_version"] = 1;
    j["queued"] = json::array();
    // If currently inflight, persist it first so interrupted runs can resume.
    if (state.inflight && !state.label.empty()) {
        j["queued"].push_back({
            {"target_dir", state.label},
            {"url_tmpl", state.active_url_tmpl}
        });
    }
    for (const auto& q : state.queue) {
        j["queued"].push_back({
            {"target_dir", q.first},
            {"url_tmpl", q.second}
        });
    }
    fs::create_directories((root / "data"));
    std::ofstream out(queueFilePath(root));
    if (out) out << j.dump(2);
}

void loadBasemapDownloadQueue(DownloadQueueState& state, const fs::path& root, std::string& status_msg) {
    if (state.loaded_from_disk) return;
    state.loaded_from_disk = true;
    std::ifstream in(queueFilePath(root));
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        status_msg = "Download queue file is invalid JSON; ignoring.";
        return;
    }
    if (!j.contains("queued") || !j["queued"].is_array()) return;
    for (const auto& row : j["queued"]) {
        if (!row.is_object()) continue;
        const std::string target = row.value("target_dir", std::string());
        const std::string tmpl = row.value("url_tmpl", std::string());
        if (target.empty() || tmpl.empty()) continue;
        state.queue.push_back({target, tmpl});
    }
    if (!state.queue.empty()) {
        status_msg = "Loaded " + std::to_string(state.queue.size()) + " queued basemap job(s) from previous run.";
        state.last_queue_event = status_msg;
    }
}

static void startBasemapDownload(
    DownloadQueueState& state,
    const fs::path& root,
    const std::string& target_dir,
    const std::string& url_tmpl,
    std::string& status_msg) {
    state.inflight = true;
    state.label = target_dir;
    state.active_url_tmpl = url_tmpl;
    state.started_at = std::chrono::steady_clock::now();
    state.stop_requested.store(false, std::memory_order_relaxed);
    state.stopped.store(false, std::memory_order_relaxed);
    state.done.store(0, std::memory_order_relaxed);
    state.total.store(0, std::memory_order_relaxed);
    state.missing_total.store(0, std::memory_order_relaxed);
    state.downloaded.store(0, std::memory_order_relaxed);
    state.skipped.store(0, std::memory_order_relaxed);
    state.failed.store(0, std::memory_order_relaxed);
    state.bytes.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.progress = "Preparing download...";
    }
    status_msg = "Basemap download started (" + target_dir + ")";

    state.worker_future = std::async(std::launch::async, [&state, root, target_dir, url_tmpl]() {
        const fs::path base = root / "data" / target_dir;
        fs::create_directories(base);
        const double min_lon = -76.75;
        const double max_lon = -76.45;
        const double min_lat = 39.18;
        const double max_lat = 39.40;
        int z_min = 8;
        int z_max = 18;
        // OpenTopoMap returns HTTP 400 for z18 in this region; avoid guaranteed failures.
        if (url_tmpl.find("tile.opentopomap.org") != std::string::npos) z_max = 17;

        size_t total = 0;
        size_t missing_total = 0;
        for (int z = z_min; z <= z_max; ++z) {
            auto [x0, y_max] = deg2num(min_lat, min_lon, z);
            auto [x1, y_min] = deg2num(max_lat, max_lon, z);
            if (x0 > x1) std::swap(x0, x1);
            if (y_min > y_max) std::swap(y_min, y_max);
            for (int x = x0; x <= x1; ++x) {
                for (int y = y_min; y <= y_max; ++y) {
                    total++;
                    std::error_code ec;
                    fs::path out = base / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png");
                    if (!(fs::exists(out, ec) && !ec)) missing_total++;
                }
            }
        }
        state.total.store(total, std::memory_order_relaxed);
        state.missing_total.store(missing_total, std::memory_order_relaxed);

        size_t done = 0;
        for (int z = z_min; z <= z_max; ++z) {
            if (state.stop_requested.load(std::memory_order_relaxed)) {
                state.stopped.store(true, std::memory_order_relaxed);
                break;
            }
            auto [x0, y_max] = deg2num(min_lat, min_lon, z);
            auto [x1, y_min] = deg2num(max_lat, max_lon, z);
            if (x0 > x1) std::swap(x0, x1);
            if (y_min > y_max) std::swap(y_min, y_max);
            for (int x = x0; x <= x1; ++x) {
                if (state.stop_requested.load(std::memory_order_relaxed)) {
                    state.stopped.store(true, std::memory_order_relaxed);
                    break;
                }
                for (int y = y_min; y <= y_max; ++y) {
                    if (state.stop_requested.load(std::memory_order_relaxed)) {
                        state.stopped.store(true, std::memory_order_relaxed);
                        break;
                    }
                    fs::path out = base / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png");
                    std::error_code ec;
                    if (fs::exists(out, ec) && !ec) {
                        state.skipped.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        std::string url = url_tmpl;
                        auto replace_all = [&](const std::string& from, const std::string& to) {
                            size_t pos = 0;
                            while ((pos = url.find(from, pos)) != std::string::npos) {
                                url.replace(pos, from.size(), to);
                                pos += to.size();
                            }
                        };
                        replace_all("{z}", std::to_string(z));
                        replace_all("{x}", std::to_string(x));
                        replace_all("{y}", std::to_string(y));
                        std::string err;
                        if (downloadUrlToFile(url, out, err)) {
                            state.downloaded.fetch_add(1, std::memory_order_relaxed);
                            std::error_code sec;
                            const auto sz = fs::file_size(out, sec);
                            if (!sec) state.bytes.fetch_add((size_t)sz, std::memory_order_relaxed);
                        } else {
                            state.failed.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    done++;
                    state.done.store(done, std::memory_order_relaxed);
                    if (done % 32 == 0 || done == total) {
                        std::lock_guard<std::mutex> lk(state.mutex);
                        state.progress =
                            "Tiles " + std::to_string(done) + "/" + std::to_string(total) +
                            " | downloaded " + std::to_string(state.downloaded.load(std::memory_order_relaxed)) +
                            ", skipped " + std::to_string(state.skipped.load(std::memory_order_relaxed)) +
                            ", failed " + std::to_string(state.failed.load(std::memory_order_relaxed));
                    }
                }
                if (state.stopped.load(std::memory_order_relaxed)) break;
            }
            if (state.stopped.load(std::memory_order_relaxed)) break;
        }
    });
}

void requestBasemapDownload(
    DownloadQueueState& state,
    const fs::path& root,
    const std::string& target_dir,
    const std::string& url_tmpl,
    std::string& status_msg) {
    if (state.inflight) {
        state.queue.push_back({target_dir, url_tmpl});
        status_msg = "Queued basemap download (" + target_dir + "). Queue size: " + std::to_string(state.queue.size());
        state.last_queue_event = status_msg;
        persistBasemapDownloadQueue(state, root);
        return;
    }
    startBasemapDownload(state, root, target_dir, url_tmpl, status_msg);
    persistBasemapDownloadQueue(state, root);
}

void tickBasemapDownloadQueue(DownloadQueueState& state, const fs::path& root, std::string& status_msg) {
    if (!state.inflight && !state.queue.empty()) {
        const auto next = state.queue.front();
        state.queue.pop_front();
        startBasemapDownload(state, root, next.first, next.second, status_msg);
        persistBasemapDownloadQueue(state, root);
    }
    if (!(state.inflight && state.worker_future.valid() &&
          state.worker_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
        return;
    }
    state.worker_future.get();
    state.inflight = false;
    const bool stopped = state.stopped.load(std::memory_order_relaxed);
    status_msg = std::string(stopped ? "Basemap download stopped (" : "Basemap download (") + state.label + "): " +
        std::to_string(state.downloaded.load(std::memory_order_relaxed)) + " downloaded, " +
        std::to_string(state.skipped.load(std::memory_order_relaxed)) + " skipped, " +
        std::to_string(state.failed.load(std::memory_order_relaxed)) + " failed";
    state.last_queue_event = status_msg;

    if (!state.queue.empty()) {
        const auto next = state.queue.front();
        state.queue.pop_front();
        startBasemapDownload(state, root, next.first, next.second, status_msg);
    }
    persistBasemapDownloadQueue(state, root);
}

void stopBasemapDownloads(DownloadQueueState& state, const fs::path& root, std::string& status_msg) {
    state.stop_requested.store(true, std::memory_order_relaxed);
    state.queue.clear();
    status_msg = "Stop requested for basemap downloads";
    state.last_queue_event = status_msg;
    persistBasemapDownloadQueue(state, root);
}

DownloadQueueMetrics snapshotDownloadQueueMetrics(DownloadQueueState& state) {
    DownloadQueueMetrics m;
    m.inflight = state.inflight;
    m.done = state.done.load(std::memory_order_relaxed);
    m.total = std::max<size_t>(1, state.total.load(std::memory_order_relaxed));
    m.missing_total = state.missing_total.load(std::memory_order_relaxed);
    m.downloaded = state.downloaded.load(std::memory_order_relaxed);
    m.skipped = state.skipped.load(std::memory_order_relaxed);
    m.failed = state.failed.load(std::memory_order_relaxed);
    m.queued = state.queue.size();
    m.label = state.label;
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        m.progress_line = state.progress;
    }
    m.progress = std::clamp((float)m.done / (float)m.total, 0.0f, 1.0f);
    const double elapsed_s = std::max(0.001, std::chrono::duration<double>(std::chrono::steady_clock::now() - state.started_at).count());
    m.tiles_per_s = (double)m.done / elapsed_s;
    m.mb_per_s = ((double)state.bytes.load(std::memory_order_relaxed) / (1024.0 * 1024.0)) / elapsed_s;
    const size_t attempted = m.downloaded + m.failed;
    const size_t remaining_missing = attempted < m.missing_total ? (m.missing_total - attempted) : 0;
    const double attempts_per_s = (double)attempted / elapsed_s;
    m.eta_s = attempts_per_s > 0.001 ? ((double)remaining_missing / attempts_per_s) : 0.0;
    m.bytes_mb = (double)state.bytes.load(std::memory_order_relaxed) / (1024.0 * 1024.0);
    return m;
}

void drawBasemapDownloadQueueSection(
    DownloadQueueState& state,
    const fs::path& root,
    std::string& status_msg,
    const std::function<std::string(const std::string&)>& resolve_label) {
    DownloadQueueMetrics m = snapshotDownloadQueueMetrics(state);
    ImGui::SeparatorText("Basemap");
    if (state.inflight) {
        std::string source_label = m.label;
        if (resolve_label) source_label = resolve_label(m.label);
        ImGui::Text("Active: %s", source_label.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("X##cancel_active_basemap")) {
            state.stop_requested.store(true, std::memory_order_relaxed);
            status_msg = "Cancel requested for active basemap job (" + source_label + ")";
            state.last_queue_event = status_msg;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("v##requeue_active_basemap")) {
            state.queue.push_back({state.label, state.active_url_tmpl});
            state.stop_requested.store(true, std::memory_order_relaxed);
            status_msg = "Moved active basemap job to bottom of queue (" + source_label + ")";
            state.last_queue_event = status_msg;
            persistBasemapDownloadQueue(state, root);
        }
        ImGui::ProgressBar(m.progress, ImVec2(420.0f, 0.0f));
        ImGui::Text("Done: %zu / %zu", m.done, m.total);
        ImGui::Text("Downloaded: %zu | Skipped: %zu | Failed: %zu", m.downloaded, m.skipped, m.failed);
        ImGui::Text("Speed: %.2f tiles/s | %.2f MB/s | ETA (missing): %.0fs", m.tiles_per_s, m.mb_per_s, m.eta_s);
        ImGui::Text("Bytes downloaded: %.2f MB", m.bytes_mb);
    } else {
        ImGui::TextDisabled("No active basemap download");
    }
    if (m.queued == 0) ImGui::TextDisabled("No queued basemap jobs");
    else {
        ImGui::TextDisabled("Queued basemap jobs: %zu", m.queued);
        size_t qi = 0;
        int remove_idx = -1;
        int move_bottom_idx = -1;
        for (const auto& q : state.queue) {
            std::string label = q.first;
            if (resolve_label) label = resolve_label(q.first);
            ImGui::PushID((int)qi);
            ImGui::Text("%zu. %s", qi + 1, label.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("X##cancel_queued")) remove_idx = (int)qi;
            ImGui::SameLine();
            if (ImGui::SmallButton("v##move_queued_bottom")) move_bottom_idx = (int)qi;
            ImGui::PopID();
            qi++;
        }
        if (remove_idx >= 0 && (size_t)remove_idx < state.queue.size()) {
            const auto canceled = state.queue[(size_t)remove_idx].first;
            state.queue.erase(state.queue.begin() + remove_idx);
            const std::string canceled_label = resolve_label ? resolve_label(canceled) : canceled;
            status_msg = "Canceled queued basemap job (" + canceled_label + ")";
            state.last_queue_event = status_msg;
            persistBasemapDownloadQueue(state, root);
        } else if (move_bottom_idx >= 0 && (size_t)move_bottom_idx < state.queue.size()) {
            auto moved = state.queue[(size_t)move_bottom_idx];
            state.queue.erase(state.queue.begin() + move_bottom_idx);
            state.queue.push_back(std::move(moved));
            const std::string moved_label = resolve_label ? resolve_label(state.queue.back().first) : state.queue.back().first;
            status_msg = "Moved queued basemap job to bottom (" + moved_label + ")";
            state.last_queue_event = status_msg;
            persistBasemapDownloadQueue(state, root);
        }
    }
    if (!state.last_queue_event.empty()) ImGui::TextDisabled("Basemap event: %s", state.last_queue_event.c_str());
    if (!m.progress_line.empty()) ImGui::TextWrapped("%s", m.progress_line.c_str());
}
