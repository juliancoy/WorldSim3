#pragma once

#include "profiling.h"

#include <mutex>
#include <vector>

struct LayerDef;
struct LayerSpatialIndex;

struct LayerProfileSnapshotRefreshContext {
    const std::vector<LayerDef>* layers = nullptr;
    const std::vector<LayerProfileAccumulator>* layer_profile_accumulators = nullptr;
    std::vector<bool>* layer_profile_dirty = nullptr;
    std::vector<LayerProfileSnapshot>* layer_profile_snapshot = nullptr;
    std::mutex* layer_profile_mutex = nullptr;
};

void refreshLayerProfileSnapshot(LayerProfileSnapshotRefreshContext& ctx);
