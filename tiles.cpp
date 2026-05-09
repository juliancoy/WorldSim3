#include "tiles.h"

#include "dataset_library.h"

#include "imgui.h"

#include <fstream>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

static fs::path lazyTileQueueFilePath(const fs::path& root) {
    return root / "data" / "lazy_tile_queue.json";
}

static fs::path tilePath(const fs::path& root, const std::string& target_dir, int z, int x, int y) {
    return root / "data" / target_dir / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png");
}

static std::string tileRequestKey(const std::string& target_dir, int z, int x, int y) {
    return target_dir + ":" + std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y);
}

static std::string tileUrlFromTemplate(std::string url, int z, int x, int y) {
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
    return url;
}

void persistLazyTileDownloadQueue(LazyTileDownloadState& state, const fs::path& root) {
    json j = json::object();
    j["schema_version"] = 1;
    j["queued"] = json::array();
    for (const auto& req : state.queue) {
        j["queued"].push_back({
            {"target_dir", req.target_dir},
            {"url_tmpl", req.url_tmpl},
            {"z", req.z},
            {"x", req.x},
            {"y", req.y}
        });
    }
    fs::create_directories(root / "data");
    std::ofstream out(lazyTileQueueFilePath(root));
    if (out) out << j.dump(2);
}

void loadLazyTileDownloadQueue(LazyTileDownloadState& state, const fs::path& root, std::string& status_msg) {
    if (state.loaded_from_disk) return;
    state.loaded_from_disk = true;
    std::ifstream in(lazyTileQueueFilePath(root));
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        status_msg = "Lazy tile queue file is invalid JSON; ignoring.";
        return;
    }
    if (!j.contains("queued") || !j["queued"].is_array()) return;
    for (const auto& row : j["queued"]) {
        if (!row.is_object()) continue;
        LazyTileDownloadRequest req;
        req.target_dir = row.value("target_dir", std::string());
        req.url_tmpl = row.value("url_tmpl", std::string());
        req.z = row.value("z", 0);
        req.x = row.value("x", 0);
        req.y = row.value("y", 0);
        if (req.target_dir.empty() || req.url_tmpl.empty()) continue;
        std::error_code ec;
        if (fs::exists(tilePath(root, req.target_dir, req.z, req.x, req.y), ec) && !ec) continue;
        const std::string key = tileRequestKey(req.target_dir, req.z, req.x, req.y);
        if (state.queued_keys.insert(key).second) state.queue.push_back(req);
    }
    if (!state.queue.empty()) {
        status_msg = "Loaded " + std::to_string(state.queue.size()) + " pending lazy tile request(s).";
    }
}

void requestLazyBasemapTile(
    LazyTileDownloadState& state,
    const fs::path& root,
    const std::string& target_dir,
    const std::string& url_tmpl,
    int z,
    int x,
    int y) {
    if (target_dir.empty() || url_tmpl.empty() || z < 0 || x < 0 || y < 0) return;
    std::error_code ec;
    if (fs::exists(tilePath(root, target_dir, z, x, y), ec) && !ec) return;
    const std::string key = tileRequestKey(target_dir, z, x, y);
    if (state.queued_keys.find(key) != state.queued_keys.end()) return;
    if (state.failed_keys.find(key) != state.failed_keys.end()) return;
    state.queue.push_back(LazyTileDownloadRequest{target_dir, url_tmpl, z, x, y});
    state.queued_keys.insert(key);
    constexpr size_t kMaxLazyTileQueue = 512;
    while (state.queue.size() > kMaxLazyTileQueue) {
        const auto dropped = state.queue.front();
        state.queue.pop_front();
        state.queued_keys.erase(tileRequestKey(dropped.target_dir, dropped.z, dropped.x, dropped.y));
    }
    persistLazyTileDownloadQueue(state, root);
}

static void startLazyTileDownload(LazyTileDownloadState& state, const fs::path& root, LazyTileDownloadRequest req) {
    state.inflight = true;
    state.started_at = std::chrono::steady_clock::now();
    state.progress = req.target_dir + " z" + std::to_string(req.z) + "/" + std::to_string(req.x) + "/" + std::to_string(req.y);
    state.worker_future = std::async(std::launch::async, [root, req]() {
        LazyTileDownloadResult result;
        result.request = req;
        const fs::path out = tilePath(root, req.target_dir, req.z, req.x, req.y);
        std::error_code ec;
        if (fs::exists(out, ec) && !ec) {
            result.ok = true;
            result.skipped = true;
            return result;
        }
        const std::string url = tileUrlFromTemplate(req.url_tmpl, req.z, req.x, req.y);
        std::string err;
        result.ok = downloadUrlToFile(url, out, err);
        result.error = err;
        if (result.ok) {
            std::error_code sec;
            const auto sz = fs::file_size(out, sec);
            if (!sec) result.bytes = (size_t)sz;
        }
        return result;
    });
}

void tickLazyTileDownloadQueue(LazyTileDownloadState& state, const fs::path& root, std::string& status_msg) {
    if (!state.inflight && !state.queue.empty()) {
        const auto req = state.queue.front();
        state.queue.pop_front();
        state.queued_keys.erase(tileRequestKey(req.target_dir, req.z, req.x, req.y));
        startLazyTileDownload(state, root, req);
        persistLazyTileDownloadQueue(state, root);
    }
    if (!(state.inflight && state.worker_future.valid() &&
          state.worker_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
        return;
    }
    LazyTileDownloadResult result = state.worker_future.get();
    state.inflight = false;
    if (result.ok) {
        if (result.skipped) state.skipped.fetch_add(1, std::memory_order_relaxed);
        else state.downloaded.fetch_add(1, std::memory_order_relaxed);
        state.bytes.fetch_add(result.bytes, std::memory_order_relaxed);
    } else {
        state.failed.fetch_add(1, std::memory_order_relaxed);
        state.failed_keys.insert(tileRequestKey(result.request.target_dir, result.request.z, result.request.x, result.request.y));
    }
    status_msg = "Lazy tile cache: " +
        std::to_string(state.downloaded.load(std::memory_order_relaxed)) + " downloaded, " +
        std::to_string(state.skipped.load(std::memory_order_relaxed)) + " skipped, " +
        std::to_string(state.failed.load(std::memory_order_relaxed)) + " failed";
    persistLazyTileDownloadQueue(state, root);
}

void drawLazyTileDownloadQueueSection(LazyTileDownloadState& state) {
    ImGui::SeparatorText("Lazy Visible Tile Cache");
    const size_t downloaded = state.downloaded.load(std::memory_order_relaxed);
    const size_t skipped = state.skipped.load(std::memory_order_relaxed);
    const size_t failed = state.failed.load(std::memory_order_relaxed);
    const double mb = (double)state.bytes.load(std::memory_order_relaxed) / (1024.0 * 1024.0);
    if (state.inflight) {
        ImGui::Text("Active: %s", state.progress.c_str());
    } else {
        ImGui::TextDisabled("No active lazy tile request");
    }
    ImGui::Text("Queued visible tiles: %zu", state.queue.size());
    ImGui::Text("Downloaded: %zu | Skipped: %zu | Failed: %zu", downloaded, skipped, failed);
    ImGui::Text("Bytes cached this run: %.2f MB", mb);
}
