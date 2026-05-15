#include "profiling_layer_snapshot.h"

#include "layer_runtime.h"
#include "types.h"

#include <algorithm>

void refreshLayerProfileSnapshot(LayerProfileSnapshotRefreshContext& ctx) {
    if (!ctx.layers || !ctx.layer_profile_accumulators || !ctx.layer_profile_dirty ||
        !ctx.layer_profile_snapshot || !ctx.layer_profile_mutex) {
        return;
    }

    bool any_dirty = false;
    for (bool dirty : *ctx.layer_profile_dirty) {
        if (dirty) {
            any_dirty = true;
            break;
        }
    }
    if (!any_dirty) return;

    const auto& layers = *ctx.layers;
    const auto& layer_profile_accumulators = *ctx.layer_profile_accumulators;
    std::vector<LayerProfileSnapshot> updates;
    updates.reserve(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        if (i >= ctx.layer_profile_dirty->size() || !(*ctx.layer_profile_dirty)[i]) continue;
        const auto& layer = layers[i];
        LayerProfileSnapshot s;
        s.index = i;
        s.name = layer.name;
        s.file = layer.file;
        s.enabled = layer.enabled;
        if (i < layer_profile_accumulators.size()) {
            const auto& acc = layer_profile_accumulators[i];
            s.features = acc.features;
            s.rings = acc.rings;
            s.ring_points = acc.ring_points;
            s.triangle_indices = acc.triangle_indices;
            s.properties = acc.properties;
            s.spatial_index_built = acc.spatial_index_built;
            s.spatial_index_cells = acc.spatial_index_cells;
            s.spatial_index_marks = acc.spatial_index_marks;
        }
        updates.push_back(std::move(s));
    }
    {
        std::lock_guard<std::mutex> lk(*ctx.layer_profile_mutex);
        for (const auto& s : updates) {
            if (s.index < ctx.layer_profile_snapshot->size()) (*ctx.layer_profile_snapshot)[s.index] = s;
        }
    }
    std::fill(ctx.layer_profile_dirty->begin(), ctx.layer_profile_dirty->end(), false);
}
