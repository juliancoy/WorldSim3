#pragma once

#include "layer_runtime.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

struct LayerPipelineDrainContext {
    std::vector<LayerDef>* layers = nullptr;
    std::deque<HydratedLayer>* hydrated_queue = nullptr;
    std::mutex* hydrated_mutex = nullptr;
    std::deque<TriJob>* tri_jobs = nullptr;
    std::deque<TriResult>* tri_results = nullptr;
    std::mutex* tri_mutex = nullptr;
    std::condition_variable* tri_cv = nullptr;
    std::vector<LayerRuntimeState>* layer_states = nullptr;
    std::mutex* status_mutex = nullptr;
    std::vector<bool>* hydration_requested = nullptr;
    std::mutex* hydrate_req_mutex = nullptr;
    std::vector<bool>* layer_profile_dirty = nullptr;
    std::atomic<size_t>* hydrated_count = nullptr;
    std::atomic<size_t>* triangulated_count = nullptr;
    int parcel_layer_idx = -1;
    bool vacant_layer_active = false;
    std::function<void()> trim_process_heap;
};

void drainHydratedLayerQueue(LayerPipelineDrainContext& ctx);
void drainTriangulationResults(LayerPipelineDrainContext& ctx);
