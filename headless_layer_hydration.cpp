#include "headless_layer_hydration.h"

#include "layer_runtime.h"
#include "layer_workers.h"
#include "memory_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {
int hydrationPriority(const LayerDef& layer) {
    if (layer.file == "regional_parcels.geojson") return 0;
    if (layer.file == "parcel.geojson") return 0;
    if (layer.file == "regional_real_property.geojson") return 1;
    if (layer.file == "real_property_information.geojson") return 1;
    if (layer.scale == "parcel") return 2;
    return 3;
}
}

bool hydrateLocalLayersHeadless(
    const fs::path& root,
    std::vector<LayerDef>& layers,
    const HeadlessLayerHydrationOptions& options,
    HeadlessLayerHydrationSummary& summary) {
    summary = {};
    summary.local_layer_count = 0;
    summary.skipped_missing_layer_count = 0;

    std::vector<size_t> local_indices;
    local_indices.reserve(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        const fs::path layer_path = root / "data" / "layers" / layers[i].file;
        const fs::path canonical_path = root / "data" / "layers" / (layers[i].file + ".canonical.bin");
        std::error_code exists_ec;
        const bool exists =
            (fs::exists(layer_path, exists_ec) && !exists_ec) ||
            (layers[i].file == "regional_parcels.geojson" && fs::exists(canonical_path, exists_ec) && !exists_ec);
        if (!exists) {
            summary.skipped_missing_layer_count += 1;
            continue;
        }
        summary.local_layer_count += 1;
        local_indices.push_back(i);
    }

    auto file_size_or_max = [&](size_t idx) {
        std::error_code ec;
        const auto size = fs::file_size(root / "data" / "layers" / layers[idx].file, ec);
        return ec ? std::numeric_limits<uintmax_t>::max() : size;
    };
    std::stable_sort(local_indices.begin(), local_indices.end(), [&](size_t a, size_t b) {
        const int ap = hydrationPriority(layers[a]);
        const int bp = hydrationPriority(layers[b]);
        if (ap != bp) return ap < bp;
        return file_size_or_max(a) < file_size_or_max(b);
    });

    summary.requested_indices = local_indices;
    summary.requested_layer_count = local_indices.size();
    if (summary.requested_layer_count == 0) return true;

    std::atomic<bool> stop{false};
    std::mutex hydrate_req_mutex;
    std::condition_variable hydrate_req_cv;
    std::deque<size_t> hydrate_requests;
    std::vector<bool> hydration_requested(layers.size(), false);
    std::vector<bool> hydration_required(layers.size(), false);
    std::mutex hydrated_mutex;
    std::deque<HydratedLayer> hydrated_queue;
    std::mutex status_mutex;
    std::vector<LayerRuntimeState> layer_states(layers.size());
    std::mutex tri_mutex;
    std::condition_variable tri_cv;
    std::deque<TriJob> tri_jobs;
    std::deque<TriResult> tri_results;

    for (size_t idx : local_indices) {
        hydrate_requests.push_back(idx);
        hydration_requested[idx] = true;
        hydration_required[idx] = true;
        layer_states[idx].status = LayerPipelineStatus::Queued;
    }

    const auto started_at = std::chrono::steady_clock::now();
    LayerWorkersContext worker_ctx{
        root,
        &layers,
        &stop,
        &hydrate_req_mutex,
        &hydrate_req_cv,
        &hydrate_requests,
        &hydration_requested,
        &hydration_required,
        &hydrated_mutex,
        &hydrated_queue,
        &status_mutex,
        &layer_states,
        &tri_mutex,
        &tri_cv,
        &tri_jobs,
        &tri_results,
        nullptr,
        nullptr,
        nullptr,
        nullptr
    };
    std::vector<std::thread> workers = startHydrationWorkers(worker_ctx, std::max(1u, options.worker_count));
    hydrate_req_cv.notify_all();

    size_t completed = 0;
    size_t failed = 0;
    while (completed + failed < summary.requested_layer_count) {
        HydratedLayer ready;
        bool have_ready = false;
        {
            std::lock_guard<std::mutex> lk(hydrated_mutex);
            if (!hydrated_queue.empty()) {
                ready = std::move(hydrated_queue.front());
                hydrated_queue.pop_front();
                have_ready = true;
            }
        }
        if (!have_ready) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (ready.index >= layers.size()) continue;
        if (!ready.features.empty()) {
            auto& dst = layers[ready.index].features;
            dst.insert(
                dst.end(),
                std::make_move_iterator(ready.features.begin()),
                std::make_move_iterator(ready.features.end()));
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                layer_states[ready.index].status = LayerPipelineStatus::Hydrating;
                layer_states[ready.index].feature_count = dst.size();
            }
        }
        if (ready.failed) {
            {
                std::lock_guard<std::mutex> lk(hydrate_req_mutex);
                hydration_requested[ready.index] = false;
            }
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                layer_states[ready.index].status = LayerPipelineStatus::Failed;
                layer_states[ready.index].error = ready.error;
            }
            summary.failures.push_back(HeadlessLayerHydrationFailure{
                ready.index,
                layers[ready.index].file,
                ready.error
            });
            failed += 1;
            if (options.verbose) {
                std::cerr << "  failed " << layers[ready.index].file << ": " << ready.error << '\n';
            }
            continue;
        }
        if (!ready.done) continue;

        {
            std::lock_guard<std::mutex> lk(hydrate_req_mutex);
            hydration_requested[ready.index] = false;
        }
        {
            std::lock_guard<std::mutex> lk(status_mutex);
            layer_states[ready.index].status = LayerPipelineStatus::Hydrated;
            layer_states[ready.index].feature_count = layers[ready.index].features.size();
            layer_states[ready.index].error.clear();
        }
        completed += 1;
        summary.total_feature_count += layers[ready.index].features.size();
        if (options.verbose) {
            std::cerr << "  [" << completed + failed << "/" << summary.requested_layer_count << "] "
                      << layers[ready.index].file << " -> " << layers[ready.index].features.size() << " features\n";
        }
        trimProcessHeap();
    }

    stop.store(true, std::memory_order_relaxed);
    hydrate_req_cv.notify_all();
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }

    summary.hydrated_layer_count = completed;
    summary.failed_layer_count = failed;
    summary.elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started_at).count();
    return failed == 0;
}
