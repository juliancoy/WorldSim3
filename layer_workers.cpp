#include "layer_workers.h"

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
                const fs::path binary_cache_path = root / "data" / "cache" / "hydration" / (layers[i].file + ".bin");
                const std::string sig = fileSignature(layer_path);
                std::error_code layer_size_ec;
                const uintmax_t layer_size_bytes = fs::file_size(layer_path, layer_size_ec);
                const bool disable_large_layer_cache =
                    !layer_size_ec &&
                    layer_size_bytes > 300ull * 1024ull * 1024ull &&
                    std::getenv("WORLD_SIM3_DISABLE_LARGE_LAYER_CACHE") != nullptr;
                const bool build_hydration_cache = !disable_large_layer_cache;
                std::vector<LayerDef::FeatureGeom> cached_features;
                bool loaded_binary_cache = false;
                bool loaded_msgpack_cache = false;
                {
                    std::lock_guard<std::mutex> lk(status_mutex);
                    if (i < layer_states.size()) {
                        layer_states[i].hydration_source_signature = sig;
                        layer_states[i].hydration_loaded_from_cache = false;
                        if (disable_large_layer_cache) {
                            layer_states[i].hydration_phase = "parsing_source_cache_disabled";
                        } else if (fs::exists(binary_cache_path)) {
                            layer_states[i].hydration_phase = "loading_binary_cache";
                        } else if (fs::exists(cache_path)) {
                            layer_states[i].hydration_phase = "loading_msgpack_cache";
                        } else {
                            layer_states[i].hydration_phase = "parsing_source_cache_missing";
                        }
                    }
                }
                if (!disable_large_layer_cache && fs::exists(binary_cache_path)) {
                    loaded_binary_cache = loadBinaryHydrationCache(binary_cache_path, sig, cached_features);
                }
                if (!loaded_binary_cache && !disable_large_layer_cache && fs::exists(cache_path)) {
                    {
                        std::lock_guard<std::mutex> lk(status_mutex);
                        if (i < layer_states.size()) layer_states[i].hydration_phase = "loading_msgpack_cache";
                    }
                    loaded_msgpack_cache = loadHydrationCache(cache_path, sig, cached_features);
                }
                if (loaded_binary_cache || loaded_msgpack_cache) {
                    const bool suspicious_cache =
                        cached_features.empty() ||
                        implausibleHydrationCache(layers[i].file, cached_features.size()) ||
                        (fs::exists(layer_path) &&
                         fs::file_size(layer_path) > 1024 * 1024 &&
                         cached_features.size() < 32);
                    if (!suspicious_cache) {
                        {
                            std::lock_guard<std::mutex> lk(status_mutex);
                            if (i < layer_states.size()) {
                                layer_states[i].hydration_phase = loaded_binary_cache
                                    ? "binary_cache_hit_queueing"
                                    : "msgpack_cache_hit_queueing";
                            }
                        }
                        if (loaded_msgpack_cache && build_hydration_cache) {
                            saveBinaryHydrationCache(binary_cache_path, sig, cached_features);
                        }
                        bool first_chunk = true;
                        for (size_t off = 0; off < cached_features.size(); off += kHydrationBatchSize) {
                            if (hydration_stop.load(std::memory_order_relaxed) || (!layers[i].enabled && !required)) break;
                            size_t end = std::min(cached_features.size(), off + kHydrationBatchSize);
                            std::vector<LayerDef::FeatureGeom> chunk;
                            chunk.reserve(end - off);
                            for (size_t k = off; k < end; ++k) chunk.push_back(std::move(cached_features[k]));
                            std::lock_guard<std::mutex> lk(hydrated_mutex);
                            hydrated_queue.push_back(HydratedLayer{
                                i,
                                std::move(chunk),
                                false,
                                false,
                                first_chunk,
                                true,
                                "",
                                sig
                            });
                            first_chunk = false;
                        }
                        std::lock_guard<std::mutex> lk(hydrated_mutex);
                        hydrated_queue.push_back(HydratedLayer{
                            i,
                            {},
                            true,
                            false,
                            first_chunk,
                            true,
                            "",
                            sig
                        });
                        releaseContainerStorage(cached_features);
                        trimProcessHeap();
                        continue;
                    } else {
                        {
                            std::lock_guard<std::mutex> lk(status_mutex);
                            if (i < layer_states.size()) layer_states[i].hydration_phase = "parsing_source_cache_rejected";
                        }
                        std::error_code ec;
                        if (loaded_binary_cache) fs::remove(binary_cache_path, ec);
                        if (loaded_msgpack_cache) fs::remove(cache_path, ec);
                        releaseContainerStorage(cached_features);
                        trimProcessHeap();
                    }
                } else {
                    std::lock_guard<std::mutex> lk(status_mutex);
                    if (i < layer_states.size() &&
                        (layer_states[i].hydration_phase == "loading_binary_cache" ||
                         layer_states[i].hydration_phase == "loading_msgpack_cache")) {
                        layer_states[i].hydration_phase = "parsing_source_cache_miss_or_stale";
                    }
                }

                std::vector<LayerDef::FeatureGeom> cache_features;
                bool hydrate_failed = false;
                bool hydrate_done = false;
                bool first_chunk = true;
                hydrateLayerBatches(
                    layer_path, kHydrationBatchSize, hydration_stop,
                    [&]() { return i < layers.size() && (layers[i].enabled || required); },
                    [&](std::vector<LayerDef::FeatureGeom>&& chunk, bool done, bool failed, const std::string& error) {
                        if (build_hydration_cache && !chunk.empty()) {
                            cache_features.insert(cache_features.end(), chunk.begin(), chunk.end());
                        }
                        hydrate_failed = hydrate_failed || failed;
                        hydrate_done = hydrate_done || done;
                        std::lock_guard<std::mutex> lk(hydrated_mutex);
                        hydrated_queue.push_back(HydratedLayer{
                            i,
                            std::move(chunk),
                            done,
                            failed,
                            first_chunk,
                            false,
                            error,
                            sig
                        });
                        first_chunk = false;
                    });
                if (hydrate_done && !hydrate_failed && !cache_features.empty() &&
                    !hydration_stop.load(std::memory_order_relaxed)) {
                    saveBinaryHydrationCache(binary_cache_path, sig, cache_features);
                    releaseContainerStorage(cache_features);
                    trimProcessHeap();
                }
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
            fs::path binary_cache_path = root / "data" / "cache" / "triangulation" / (job.file + ".tri.bin");
            std::string sig = job.source_signature.empty() ? fileSignature(layer_path) : job.source_signature;
            TriResult result;
            result.index = job.index;
            result.source_signature = sig;
            try {
                bool loaded_binary_cache = false;
                bool loaded_json_cache = false;
                {
                    std::lock_guard<std::mutex> lk(status_mutex);
                    if (job.index < layer_states.size()) {
                        layer_states[job.index].triangulation_source_signature = sig;
                        layer_states[job.index].triangulation_loaded_from_cache = false;
                        if (fs::exists(binary_cache_path)) {
                            layer_states[job.index].triangulation_phase = "loading_binary_cache";
                        } else if (fs::exists(cache_path)) {
                            layer_states[job.index].triangulation_phase = "loading_json_cache";
                        } else {
                            layer_states[job.index].triangulation_phase = "building_cache_missing";
                        }
                    }
                }
                loaded_binary_cache = loadBinaryTriCache(binary_cache_path, sig, job.rings_per_feature.size(), result.triangles_per_feature);
                if (!loaded_binary_cache && fs::exists(cache_path)) {
                    {
                        std::lock_guard<std::mutex> lk(status_mutex);
                        if (job.index < layer_states.size()) layer_states[job.index].triangulation_phase = "loading_json_cache";
                    }
                    loaded_json_cache = loadTriCache(cache_path, sig, job.rings_per_feature.size(), result.triangles_per_feature);
                }
                if (!loaded_binary_cache && !loaded_json_cache) {
                    {
                        std::lock_guard<std::mutex> lk(status_mutex);
                        if (job.index < layer_states.size()) {
                            layer_states[job.index].triangulation_phase = "building_source_triangles";
                        }
                    }
                    result.triangles_per_feature.resize(job.rings_per_feature.size());
                    for (size_t i = 0; i < job.rings_per_feature.size(); ++i) {
                        if (!job.rings_per_feature[i].empty()) {
                            result.triangles_per_feature[i] = triangulateRings(job.rings_per_feature[i]);
                        }
                    }
                    saveBinaryTriCache(binary_cache_path, sig, result.triangles_per_feature);
                } else {
                    result.loaded_from_cache = true;
                    result.loaded_from_binary_cache = loaded_binary_cache;
                    {
                        std::lock_guard<std::mutex> lk(status_mutex);
                        if (job.index < layer_states.size()) {
                            layer_states[job.index].triangulation_phase = loaded_binary_cache
                                ? "binary_cache_hit"
                                : "json_cache_hit";
                        }
                    }
                    if (loaded_json_cache) {
                        saveBinaryTriCache(binary_cache_path, sig, result.triangles_per_feature);
                    }
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
            releaseContainerStorage(job.rings_per_feature);
            trimProcessHeap();
        }
    
    });
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
