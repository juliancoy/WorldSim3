#include "layer_pipeline_drain.h"

#include "memory_utils.h"

#include <algorithm>
#include <chrono>
#include <iterator>

namespace {

const char* completedTriangulationPhase(const TriResult& tr) {
    if (!tr.loaded_from_cache) return "built_from_source";
    return tr.loaded_from_binary_cache ? "binary_cache_hit" : "json_cache_hit";
}

const char* applyingTriangulationPhase(const TriResult& tr) {
    if (!tr.loaded_from_cache) return "applying_built_result";
    return tr.loaded_from_binary_cache ? "applying_binary_cache" : "applying_json_cache";
}

}  // namespace

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
                if (ctx.layer_fallback_scan_cursor && ready.index < ctx.layer_fallback_scan_cursor->size()) {
                    (*ctx.layer_fallback_scan_cursor)[ready.index] = 0;
                }
                if (ctx.layer_profile_accumulators && ready.index < ctx.layer_profile_accumulators->size()) {
                    (*ctx.layer_profile_accumulators)[ready.index] = LayerProfileAccumulator{};
                }
                if (ready.index < ctx.layer_profile_dirty->size()) (*ctx.layer_profile_dirty)[ready.index] = true;
                std::lock_guard<std::mutex> lk3(*ctx.status_mutex);
                if (ready.index < ctx.layer_states->size()) {
                    source_signature_changed =
                        (*ctx.layer_states)[ready.index].hydration_source_signature != ready.source_signature;
                    (*ctx.layer_states)[ready.index].feature_count = 0;
                    (*ctx.layer_states)[ready.index].hydration_source_signature = ready.source_signature;
                    (*ctx.layer_states)[ready.index].triangulation_source_signature.clear();
                    (*ctx.layer_states)[ready.index].spatial_index_source_signature.clear();
                    (*ctx.layer_states)[ready.index].hydration_loaded_from_cache = ready.loaded_from_cache;
                    (*ctx.layer_states)[ready.index].hydration_phase = ready.loaded_from_cache ? "cache_hit" : "source_parse";
                    (*ctx.layer_states)[ready.index].spatial_index_phase.clear();
                }
                if (source_signature_changed && ctx.projection_cache_generation) {
                    *ctx.projection_cache_generation += 1;
                }
            }
            if (!ready.features.empty()) {
                auto& dst = (*ctx.layers)[ready.index].features;
                if (ctx.layer_profile_accumulators && ready.index < ctx.layer_profile_accumulators->size()) {
                    auto& acc = (*ctx.layer_profile_accumulators)[ready.index];
                    for (const auto& fg : ready.features) {
                        acc.features += 1;
                        acc.rings += fg.rings.size();
                        acc.properties += fg.properties.size();
                        for (const auto& r : fg.rings) acc.ring_points += r.size();
                    }
                }
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

    constexpr size_t kMaxTriangulationApplyFeaturesPerFrame = 4096;
    constexpr double kMaxTriangulationApplyMs = 4.0;
    const auto drain_start = std::chrono::steady_clock::now();
    size_t applied_features = 0;
    while (true) {
        TriResult tr;
        {
            std::lock_guard<std::mutex> lk(*ctx.tri_mutex);
            if (ctx.tri_results->empty()) break;
            tr = std::move(ctx.tri_results->front());
            ctx.tri_results->pop_front();
        }
        if (tr.index < ctx.layers->size()) {
            if (tr.ok) {
                auto& fs = (*ctx.layers)[tr.index].features;
                const size_t n = std::min(fs.size(), tr.triangles_per_feature.size());
                const size_t remaining_budget = kMaxTriangulationApplyFeaturesPerFrame - applied_features;
                const size_t end = std::min(n, tr.apply_offset + remaining_budget);
                for (size_t i = tr.apply_offset; i < end; ++i) {
                    if (ctx.layer_profile_accumulators && tr.index < ctx.layer_profile_accumulators->size()) {
                        auto& acc = (*ctx.layer_profile_accumulators)[tr.index];
                        if (acc.triangle_indices >= fs[i].triangles.size()) {
                            acc.triangle_indices -= fs[i].triangles.size();
                        } else {
                            acc.triangle_indices = 0;
                        }
                        acc.triangle_indices += tr.triangles_per_feature[i].size();
                    }
                    fs[i].triangles = std::move(tr.triangles_per_feature[i]);
                }
                if (tr.index < ctx.layer_profile_dirty->size()) (*ctx.layer_profile_dirty)[tr.index] = true;
                applied_features += (end - tr.apply_offset);
                tr.apply_offset = end;
                if (tr.apply_offset < n) {
                    {
                        std::lock_guard<std::mutex> lk3(*ctx.status_mutex);
                        if (tr.index < ctx.layer_states->size()) {
                            (*ctx.layer_states)[tr.index].status = LayerPipelineStatus::Triangulating;
                            (*ctx.layer_states)[tr.index].triangulation_source_signature = tr.source_signature;
                            (*ctx.layer_states)[tr.index].triangulation_loaded_from_cache = tr.loaded_from_cache;
                            (*ctx.layer_states)[tr.index].triangulation_phase = applyingTriangulationPhase(tr);
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lk(*ctx.tri_mutex);
                        ctx.tri_results->push_front(std::move(tr));
                    }
                } else {
                    ctx.triangulated_count->fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lk3(*ctx.status_mutex);
                    if (tr.index < ctx.layer_states->size()) {
                        (*ctx.layer_states)[tr.index].status = LayerPipelineStatus::Ready;
                        (*ctx.layer_states)[tr.index].triangulation_source_signature = tr.source_signature;
                        (*ctx.layer_states)[tr.index].triangulation_loaded_from_cache = tr.loaded_from_cache;
                        (*ctx.layer_states)[tr.index].triangulation_phase = completedTriangulationPhase(tr);
                    }
                    if (ctx.trim_process_heap) ctx.trim_process_heap();
                }
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
        const double drain_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - drain_start).count();
        if (applied_features >= kMaxTriangulationApplyFeaturesPerFrame || drain_ms >= kMaxTriangulationApplyMs) {
            break;
        }
    }
}

void drainSpatialIndexResults(LayerPipelineDrainContext& ctx) {
    if (!ctx.layers || !ctx.spatial_results || !ctx.spatial_mutex || !ctx.layer_states ||
        !ctx.layer_spatial || !ctx.status_mutex || !ctx.layer_profile_dirty) {
        return;
    }

    constexpr size_t kMaxSpatialResultsPerFrame = 8;
    size_t drained = 0;
    while (drained < kMaxSpatialResultsPerFrame) {
        SpatialIndexResult result;
        {
            std::lock_guard<std::mutex> lk(*ctx.spatial_mutex);
            if (ctx.spatial_results->empty()) break;
            result = std::move(ctx.spatial_results->front());
            ctx.spatial_results->pop_front();
        }

        if (result.index >= ctx.layers->size()) {
            drained++;
            continue;
        }

        const size_t current_feature_count = (*ctx.layers)[result.index].features.size();
        std::string current_source_signature;
        {
            std::lock_guard<std::mutex> lk(*ctx.status_mutex);
            if (result.index < ctx.layer_states->size()) {
                current_source_signature = (*ctx.layer_states)[result.index].hydration_source_signature;
            }
        }
        const bool stale_result =
            !result.ok ||
            result.feature_count != current_feature_count ||
            current_source_signature != result.source_signature;

        {
            std::lock_guard<std::mutex> lk(*ctx.status_mutex);
            if (result.index < ctx.layer_states->size()) {
                if (!result.ok) {
                    (*ctx.layer_states)[result.index].spatial_index_phase = "failed";
                } else if (stale_result) {
                    (*ctx.layer_states)[result.index].spatial_index_phase = "stale_discarded";
                } else {
                    (*ctx.layer_states)[result.index].spatial_index_source_signature = result.source_signature;
                    (*ctx.layer_states)[result.index].spatial_index_phase = "ready";
                }
            }
        }
        if (ctx.spatial_index_requested_feature_count &&
            result.index < ctx.spatial_index_requested_feature_count->size()) {
            (*ctx.spatial_index_requested_feature_count)[result.index] = 0;
        }
        if (ctx.spatial_index_requested_signature &&
            result.index < ctx.spatial_index_requested_signature->size()) {
            (*ctx.spatial_index_requested_signature)[result.index].clear();
        }
        if (!result.ok || stale_result) {
            drained++;
            continue;
        }

        (*ctx.layer_spatial)[result.index] = std::move(result.spatial_index);
        if (ctx.layer_profile_accumulators &&
            result.index < ctx.layer_profile_accumulators->size() &&
            result.index < ctx.layer_spatial->size()) {
            auto& acc = (*ctx.layer_profile_accumulators)[result.index];
            const auto& si = (*ctx.layer_spatial)[result.index];
            acc.spatial_index_built = si.built;
            acc.spatial_index_cells = si.cells.size();
            acc.spatial_index_marks = si.marks.size();
        }
        if (result.index < ctx.layer_profile_dirty->size()) {
            (*ctx.layer_profile_dirty)[result.index] = true;
        }
        drained++;
    }
}
