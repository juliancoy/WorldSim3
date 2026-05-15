#pragma once

#include "layer_runtime.h"
#include "types.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

struct LayerWorkersContext {
    std::filesystem::path root;
    std::vector<LayerDef>* layers = nullptr;
    std::atomic<bool>* stop = nullptr;

    std::mutex* hydrate_req_mutex = nullptr;
    std::condition_variable* hydrate_req_cv = nullptr;
    std::deque<size_t>* hydrate_requests = nullptr;
    std::vector<bool>* hydration_requested = nullptr;
    std::vector<bool>* hydration_required = nullptr;

    std::mutex* hydrated_mutex = nullptr;
    std::deque<HydratedLayer>* hydrated_queue = nullptr;

    std::mutex* status_mutex = nullptr;
    std::vector<LayerRuntimeState>* layer_states = nullptr;

    std::mutex* tri_mutex = nullptr;
    std::condition_variable* tri_cv = nullptr;
    std::deque<TriJob>* tri_jobs = nullptr;
    std::deque<TriResult>* tri_results = nullptr;

    std::mutex* spatial_mutex = nullptr;
    std::condition_variable* spatial_cv = nullptr;
    std::deque<SpatialIndexJob>* spatial_jobs = nullptr;
    std::deque<SpatialIndexResult>* spatial_results = nullptr;
};

std::vector<std::thread> startHydrationWorkers(LayerWorkersContext ctx, unsigned int worker_count);
std::thread startTriangulationWorker(LayerWorkersContext ctx);
std::thread startSpatialIndexWorker(LayerWorkersContext ctx);
