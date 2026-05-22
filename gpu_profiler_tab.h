#pragma once

#include "profiling.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

struct GpuProfilerTabContext {
    std::mutex* profile_mutex = nullptr;
    std::vector<ProfileFrameSample>* profile_samples = nullptr;
    size_t* profile_sample_pos = nullptr;
    size_t* profile_sample_count = nullptr;
    std::atomic<bool>* prof_heatmap_gpu_splat_active = nullptr;
    std::atomic<bool>* prof_heatmap_high_quality = nullptr;
    std::atomic<bool>* prof_heatmap_texture_resident = nullptr;
    std::atomic<bool>* prof_heatmap_async_inflight = nullptr;
    std::atomic<size_t>* prof_heatmap_texture_cache_entries = nullptr;
    bool* tab_requested = nullptr;
    bool* reload_requested = nullptr;
};

void drawGpuProfilerTab(const GpuProfilerTabContext& ctx);
