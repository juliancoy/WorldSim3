#include "layer_pipeline_drain.h"

#include "memory_utils.h"

#include <algorithm>
#include <chrono>
#include <iterator>

void drainHydratedLayerQueue(LayerPipelineDrainContext& ctx) {
    if (!ctx.layers || !ctx.hydrated_queue || !ctx.hydrated_mutex || !ctx.tri_jobs || !ctx.tri_mutex ||
        !ctx.tri_cv || !ctx.layer_states || !ctx.status_mutex || !ctx.hydration_requested ||
        !ctx.hydrate_req_mutex || !ctx.layer_profile_dirty || !ctx.hydrated_count) {
        return;
    }

    constexpr size_t kMaxHydrationChunksPerFrame = 24;
    constexpr double kMaxHydrationDrainMs = 4.0;
    const auto hydration_drain_start = std::chrono::steady_clock::now();
    size_t hydration_chunks_drained = 0;
    while (true) {
        HydratedLayer ready;
        {
            std::lock_guard<std::mutex> lk(*ctx.hydrated_mutex);
            if (ctx.hydrated_queue->empty()) break;
            ready = std::move(ctx.hydrated_queue->front());
            ctx.hydrated_queue->pop_front();
        }
        if (ready.index < ctx.layers->size()) {
            bool source_signature_changed = false;
            if (ready.replace_existing) {
                releaseContainerStorage((*ctx.layers)[ready.index].features);
                if (ctx.layer_spatial && ready.index < ctx.layer_spatial->size()) {
                    (*ctx.layer_spatial)[ready.index] = LayerSpatialIndex{};
                }
                if (ready.index < ctx.layer_profile_dirty->size()) (*ctx.layer_profile_dirty)[ready.index] = true;
                std::lock_guard<std::mutex> lk3(*ctx.status_mutex);
                if (ready.index < ctx.layer_states->size()) {
                    source_signature_changed =
                        (*ctx.layer_states)[ready.index].hydration_source_signature != ready.source_signature;
                    (*ctx.layer_states)[ready.index].feature_count = 0;
                    (*ctx.layer_states)[ready.index].hydration_source_signature = ready.source_signature;
                    (*ctx.layer_states)[ready.index].triangulation_source_signature.clear();
                    (*ctx.layer_states)[ready.index].hydration_loaded_from_cache = ready.loaded_from_cache;
                    (*ctx.layer_states)[ready.index].hydration_phase = ready.loaded_from_cache ? "cache_hit" : "source_parse";
                }
                if (source_signature_changed && ctx.projection_cache_generation) {
                    *ctx.projection_cache_generation += 1;
                }
            }
            if (!ready.features.empty()) {
                auto& dst = (*ctx.layers)[ready.index].features;
                dst.insert(
                    dst.end(),
                    std::make_move_iterator(ready.features.begin()),
                    std::make_move_iterator(ready.features.end()));
                if (ready.index < ctx.layer_profile_dirty->size()) (*ctx.layer_profile_dirty)[ready.index] = true;
                std::lock_guard<std::mutex> lk3(*ctx.status_mutex);
                if (ready.index < ctx.layer_states->size()) {
                    (*ctx.layer_states)[ready.index].status = LayerPipelineStatus::Hydrating;
                    (*ctx.layer_states)[ready.index].feature_count = (*ctx.layers)[ready.index].features.size();
                    (*ctx.layer_states)[ready.index].hydration_source_signature = ready.source_signature;
                    (*ctx.layer_states)[ready.index].hydration_loaded_from_cache = ready.loaded_from_cache;
                    (*ctx.layer_states)[ready.index].hydration_phase = ready.loaded_from_cache ? "cache_hit" : "source_parse";
                }
            }
            if (ready.failed) {
                {
                    std::lock_guard<std::mutex> lk4(*ctx.hydrate_req_mutex);
                    if (ready.index < ctx.hydration_requested->size()) (*ctx.hydration_requested)[ready.index] = false;
                }
                std::lock_guard<std::mutex> lk3(*ctx.status_mutex);
                if (ready.index < ctx.layer_states->size()) {
                    (*ctx.layer_states)[ready.index].status = LayerPipelineStatus::Failed;
                    (*ctx.layer_states)[ready.index].error = ready.error;
                    (*ctx.layer_states)[ready.index].hydration_source_signature = ready.source_signature;
                    (*ctx.layer_states)[ready.index].triangulation_source_signature.clear();
                    (*ctx.layer_states)[ready.index].hydration_phase = "failed";
                }
                continue;
            }
            if (!ready.done) continue;
            {
                std::lock_guard<std::mutex> lk4(*ctx.hydrate_req_mutex);
                if (ready.index < ctx.hydration_requested->size()) (*ctx.hydration_requested)[ready.index] = false;
            }
            ctx.hydrated_count->fetch_add(1, std::memory_order_relaxed);
            if (ctx.duckdb_auto_rebuild_checked && source_signature_changed) {
                *ctx.duckdb_auto_rebuild_checked = false;
            }
            {
                std::lock_guard<std::mutex> lk3(*ctx.status_mutex);
                if (ready.index < ctx.layer_states->size()) {
                    (*ctx.layer_states)[ready.index].status = LayerPipelineStatus::Hydrated;
                    (*ctx.layer_states)[ready.index].feature_count = (*ctx.layers)[ready.index].features.size();
                    (*ctx.layer_states)[ready.index].error.clear();
                    (*ctx.layer_states)[ready.index].hydration_source_signature = ready.source_signature;
                    (*ctx.layer_states)[ready.index].hydration_loaded_from_cache = ready.loaded_from_cache;
                    (*ctx.layer_states)[ready.index].hydration_phase = ready.loaded_from_cache ? "cache_hit" : "source_parse";
                }
            }
            TriJob tj;
            tj.index = ready.index;
            tj.file = (*ctx.layers)[ready.index].file;
            tj.source_signature = ready.source_signature;
            tj.rings_per_feature.reserve((*ctx.layers)[ready.index].features.size());
            for (const auto& fg : (*ctx.layers)[ready.index].features) tj.rings_per_feature.push_back(fg.rings);
            std::lock_guard<std::mutex> lk2(*ctx.tri_mutex);
            {
                std::lock_guard<std::mutex> lk3(*ctx.status_mutex);
                if (ready.index < ctx.layer_states->size()) (*ctx.layer_states)[ready.index].status = LayerPipelineStatus::TriQueued;
            }
            const bool parcel_dep_priority =
                ctx.vacant_layer_active && ctx.parcel_layer_idx >= 0 && (int)ready.index == ctx.parcel_layer_idx;
            if ((*ctx.layers)[ready.index].enabled || parcel_dep_priority) {
                ctx.tri_jobs->push_front(std::move(tj));
            } else {
                ctx.tri_jobs->push_back(std::move(tj));
            }
            ctx.tri_cv->notify_one();
            if (ctx.trim_process_heap) ctx.trim_process_heap();
        }
        hydration_chunks_drained++;
        const double drain_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - hydration_drain_start).count();
        if (hydration_chunks_drained >= kMaxHydrationChunksPerFrame || drain_ms >= kMaxHydrationDrainMs) {
            break;
        }
    }
}

void drainTriangulationResults(LayerPipelineDrainContext& ctx) {
    if (!ctx.layers || !ctx.tri_results || !ctx.tri_mutex || !ctx.layer_states ||
        !ctx.status_mutex || !ctx.layer_profile_dirty || !ctx.triangulated_count) {
        return;
    }

    std::lock_guard<std::mutex> lk(*ctx.tri_mutex);
    while (!ctx.tri_results->empty()) {
        TriResult tr = std::move(ctx.tri_results->front());
        ctx.tri_results->pop_front();
        if (tr.index < ctx.layers->size()) {
            if (tr.ok) {
                auto& fs = (*ctx.layers)[tr.index].features;
                size_t n = std::min(fs.size(), tr.triangles_per_feature.size());
                for (size_t i = 0; i < n; ++i) fs[i].triangles = std::move(tr.triangles_per_feature[i]);
                if (tr.index < ctx.layer_profile_dirty->size()) (*ctx.layer_profile_dirty)[tr.index] = true;
                ctx.triangulated_count->fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lk3(*ctx.status_mutex);
                if (tr.index < ctx.layer_states->size()) {
                    (*ctx.layer_states)[tr.index].status = LayerPipelineStatus::Ready;
                    (*ctx.layer_states)[tr.index].triangulation_source_signature = tr.source_signature;
                    (*ctx.layer_states)[tr.index].triangulation_loaded_from_cache = tr.loaded_from_cache;
                    (*ctx.layer_states)[tr.index].triangulation_phase =
                        tr.loaded_from_cache
                            ? (tr.loaded_from_binary_cache ? "binary_cache_hit" : "json_cache_hit")
                            : "built_from_source";
                }
                if (ctx.trim_process_heap) ctx.trim_process_heap();
            } else {
                std::lock_guard<std::mutex> lk3(*ctx.status_mutex);
                if (tr.index < ctx.layer_states->size()) {
                    (*ctx.layer_states)[tr.index].status = LayerPipelineStatus::Failed;
                    (*ctx.layer_states)[tr.index].error = tr.error;
                    (*ctx.layer_states)[tr.index].triangulation_source_signature.clear();
                    (*ctx.layer_states)[tr.index].triangulation_phase = "failed";
                    (*ctx.layer_states)[tr.index].triangulation_loaded_from_cache = false;
                }
            }
        }
    }
}
