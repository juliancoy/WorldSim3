#include "layer_workers.h"

#include "app_utils.h"
#include "cache_io.h"
#include "layer_geometry.h"
#include "memory_utils.h"
#include "thread_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <system_error>

namespace fs = std::filesystem;

std::vector<std::thread> startHydrationWorkers(LayerWorkersContext ctx, unsigned int worker_count) {
    (void)ctx;
    (void)worker_count;
    return {};
}

std::thread startSpatialIndexWorker(LayerWorkersContext ctx) {
    return std::thread([ctx]() mutable {
        setCurrentThreadName("ws3-spatial-index");
        auto& hydration_stop = *ctx.stop;
        auto& spatial_mutex = *ctx.spatial_mutex;
        auto& spatial_cv = *ctx.spatial_cv;
        auto& spatial_jobs = *ctx.spatial_jobs;
        auto& spatial_results = *ctx.spatial_results;
        auto& status_mutex = *ctx.status_mutex;
        auto& layer_states = *ctx.layer_states;

        while (!hydration_stop.load(std::memory_order_relaxed)) {
            SpatialIndexJob job;
            {
                std::unique_lock<std::mutex> lk(spatial_mutex);
                spatial_cv.wait_for(lk, std::chrono::milliseconds(100), [&]() {
                    return hydration_stop.load(std::memory_order_relaxed) || !spatial_jobs.empty();
                });
                if (hydration_stop.load(std::memory_order_relaxed)) break;
                if (spatial_jobs.empty()) continue;
                job = std::move(spatial_jobs.front());
                spatial_jobs.pop_front();
            }
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if (job.index < layer_states.size()) {
                    layer_states[job.index].spatial_index_source_signature = job.source_signature;
                    layer_states[job.index].spatial_index_phase = "building";
                }
            }

            SpatialIndexResult result;
            result.index = job.index;
            result.source_signature = job.source_signature;
            result.feature_count = job.feature_extents.size();
            try {
                buildLayerSpatialIndexForExtents(job.feature_extents, result.spatial_index);
                result.ok = true;
            } catch (const std::exception& e) {
                result.ok = false;
                result.error = e.what();
            }
            releaseContainerStorage(job.feature_extents);
            {
                std::lock_guard<std::mutex> lk(spatial_mutex);
                spatial_results.push_back(std::move(result));
            }
            trimProcessHeap();
        }
    });
}
