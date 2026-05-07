#include "layer_workers.h"

#include "cache_io.h"
#include "layer_geometry.h"
#include "thread_utils.h"

#include <algorithm>
#include <chrono>
#include <system_error>

namespace fs = std::filesystem;

std::vector<std::thread> startHydrationWorkers(LayerWorkersContext ctx, unsigned int worker_count) {
    std::vector<std::thread> workers;
    for (unsigned int wi = 0; wi < worker_count; ++wi) {
        workers.emplace_back([ctx, wi]() mutable {
            std::string thread_name = "ws3-hydrate-" + std::to_string(wi);
            setCurrentThreadName(thread_name.c_str());
            auto& root = ctx.root;
            auto& layers = *ctx.layers;
            auto& hydration_stop = *ctx.stop;
            auto& hydrate_req_mutex = *ctx.hydrate_req_mutex;
            auto& hydrate_req_cv = *ctx.hydrate_req_cv;
            auto& hydrate_requests = *ctx.hydrate_requests;
            auto& hydration_required = *ctx.hydration_required;
            auto& hydrated_mutex = *ctx.hydrated_mutex;
            auto& hydrated_queue = *ctx.hydrated_queue;
            auto& status_mutex = *ctx.status_mutex;
            auto& layer_states = *ctx.layer_states;

            constexpr size_t kHydrationBatchSize = 350;
            while (!hydration_stop.load(std::memory_order_relaxed)) {
                size_t i = 0;
                {
                    std::unique_lock<std::mutex> lk(hydrate_req_mutex);
                    hydrate_req_cv.wait_for(lk, std::chrono::milliseconds(100), [&]() {
                        return hydration_stop.load(std::memory_order_relaxed) || !hydrate_requests.empty();
                    });
                    if (hydration_stop.load(std::memory_order_relaxed)) break;
                    if (hydrate_requests.empty()) continue;
                    i = hydrate_requests.front();
                    hydrate_requests.pop_front();
                }
                bool required = false;
                {
                    std::lock_guard<std::mutex> lk(hydrate_req_mutex);
                    required = i < hydration_required.size() && hydration_required[i];
                }
                if (i >= layers.size() || (!layers[i].enabled && !required)) continue;
                {
                    std::lock_guard<std::mutex> lk(status_mutex);
                    if (i < layer_states.size()) layer_states[i].status = LayerPipelineStatus::Hydrating;
                }

                const fs::path layer_path = root / "data" / "layers" / layers[i].file;
                const fs::path cache_path = root / "data" / "cache" / "hydration" / (layers[i].file + ".msgpack");
                const std::string sig = fileSignature(layer_path);
                std::vector<LayerDef::FeatureGeom> cached_features;
                if (loadHydrationCache(cache_path, sig, cached_features)) {
                    const bool suspicious_cache =
                        cached_features.empty() ||
                        implausibleHydrationCache(layers[i].file, cached_features.size()) ||
                        (fs::exists(layer_path) &&
                         fs::file_size(layer_path) > 1024 * 1024 &&
                         cached_features.size() < 32);
                    if (!suspicious_cache) {
                        for (size_t off = 0; off < cached_features.size(); off += kHydrationBatchSize) {
                            if (hydration_stop.load(std::memory_order_relaxed) || (!layers[i].enabled && !required)) break;
                            size_t end = std::min(cached_features.size(), off + kHydrationBatchSize);
                            std::vector<LayerDef::FeatureGeom> chunk;
                            chunk.reserve(end - off);
                            for (size_t k = off; k < end; ++k) chunk.push_back(std::move(cached_features[k]));
                            std::lock_guard<std::mutex> lk(hydrated_mutex);
                            hydrated_queue.push_back(HydratedLayer{i, std::move(chunk), false, false, ""});
                        }
                        std::lock_guard<std::mutex> lk(hydrated_mutex);
                        hydrated_queue.push_back(HydratedLayer{i, {}, true, false, ""});
                        continue;
                    } else {
                        std::error_code ec;
                        fs::remove(cache_path, ec);
                    }
                }

                hydrateLayerBatches(
                    layer_path, kHydrationBatchSize, hydration_stop,
                    [&]() { return i < layers.size() && (layers[i].enabled || required); },
                    [&](std::vector<LayerDef::FeatureGeom>&& chunk, bool done, bool failed, const std::string& error) {
                        std::lock_guard<std::mutex> lk(hydrated_mutex);
                        hydrated_queue.push_back(HydratedLayer{i, std::move(chunk), done, failed, error});
                    });
            }
        });
    }

    return workers;
}

std::thread startTriangulationWorker(LayerWorkersContext ctx) {
    return std::thread([ctx]() mutable {
    setCurrentThreadName("ws3-triangulate");

    auto& root = ctx.root;
    auto& layers = *ctx.layers;
    auto& hydration_stop = *ctx.stop;
    auto& hydrate_req_mutex = *ctx.hydrate_req_mutex;
    auto& hydrate_req_cv = *ctx.hydrate_req_cv;
    auto& hydrate_requests = *ctx.hydrate_requests;
    auto& hydration_requested = *ctx.hydration_requested;
    auto& hydration_required = *ctx.hydration_required;
    auto& hydrated_mutex = *ctx.hydrated_mutex;
    auto& hydrated_queue = *ctx.hydrated_queue;
    auto& status_mutex = *ctx.status_mutex;
    auto& layer_states = *ctx.layer_states;
    auto& tri_mutex = *ctx.tri_mutex;
    auto& tri_cv = *ctx.tri_cv;
    auto& tri_jobs = *ctx.tri_jobs;
    auto& tri_results = *ctx.tri_results;

        while (!hydration_stop.load(std::memory_order_relaxed)) {
            TriJob job;
            {
                std::unique_lock<std::mutex> lk(tri_mutex);
                tri_cv.wait_for(lk, std::chrono::milliseconds(100), [&]() {
                    return hydration_stop.load(std::memory_order_relaxed) || !tri_jobs.empty();
                });
                if (hydration_stop.load(std::memory_order_relaxed)) break;
                if (tri_jobs.empty()) continue;
                    job = std::move(tri_jobs.front());
                    tri_jobs.pop_front();
            }
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if (job.index < layer_states.size()) layer_states[job.index].status = LayerPipelineStatus::Triangulating;
            }

            fs::path layer_path = root / "data" / "layers" / job.file;
            fs::path cache_path = root / "data" / "cache" / "triangulation" / (job.file + ".tri.json");
            std::string sig = fileSignature(layer_path);
            TriResult result;
            result.index = job.index;
            try {
                if (!loadTriCache(cache_path, sig, job.rings_per_feature.size(), result.triangles_per_feature)) {
                    result.triangles_per_feature.resize(job.rings_per_feature.size());
                    for (size_t i = 0; i < job.rings_per_feature.size(); ++i) {
                        if (!job.rings_per_feature[i].empty()) {
                            result.triangles_per_feature[i] = triangulateRings(job.rings_per_feature[i]);
                        }
                    }
                    saveTriCache(cache_path, sig, result.triangles_per_feature);
                }
                result.ok = true;
            } catch (const std::exception& e) {
                result.ok = false;
                result.error = e.what();
            }
            {
                std::lock_guard<std::mutex> lk(tri_mutex);
                tri_results.push_back(std::move(result));
            }
        }
    
    });
}
