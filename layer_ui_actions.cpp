#include "layer_ui_actions.h"

#include "aggregate_visualization_strategies.h"
#include "dataset_library.h"
#include "layer_import.h"
#include "memory_utils.h"

#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

namespace {
void clearParcelHeatmapLayers(LayerUiSharedContext& ctx) {
    if (!ctx.layers) return;
    for (auto& layer : *ctx.layers) {
        if (layer.scale == "parcel" && !layer.heatmap_field.empty()) layer.enabled = false;
    }
}

void setFreshness(
    LayerUiSharedContext& ctx,
    size_t idx,
    FreshnessState state,
    const std::string& message) {
    if (ctx.data_freshness_state && idx < ctx.data_freshness_state->size()) {
        (*ctx.data_freshness_state)[idx] = state;
    }
    if (ctx.data_freshness_msg && idx < ctx.data_freshness_msg->size()) {
        (*ctx.data_freshness_msg)[idx] = message;
    }
}
}

int findLayerFile(const LayerUiSharedContext& ctx, std::string_view file) {
    if (ctx.layer_registry) return ctx.layer_registry->findLayerByFile(file);
    if (!ctx.layers) return -1;
    for (size_t i = 0; i < ctx.layers->size(); ++i) {
        if ((*ctx.layers)[i].file == file) return (int)i;
    }
    return -1;
}

bool hiddenParcelParameterLayer(const LayerUiSharedContext& ctx, int parcel_layer_idx, size_t idx) {
    if (ctx.layer_registry) return ctx.layer_registry->isHiddenParcelGeometryLayer(idx);
    return ctx.layers &&
           parcel_layer_idx >= 0 &&
           idx < ctx.layers->size() &&
           (int)idx != parcel_layer_idx &&
           (*ctx.layers)[idx].scale == "parcel" &&
           (*ctx.layers)[idx].region.empty();
}

void setParcelParameterMode(LayerUiSharedContext& ctx, int mode) {
    if (ctx.parcel_parameter_mode) *ctx.parcel_parameter_mode = mode;
    clearParcelHeatmapLayers(ctx);
}

void activateParameterLayer(LayerUiSharedContext& ctx, int layer_idx) {
    setParcelParameterMode(ctx, 0);
    if (!ctx.layers || layer_idx < 0 || (size_t)layer_idx >= ctx.layers->size()) return;
    (*ctx.layers)[(size_t)layer_idx].enabled = true;
    if (ctx.layer_heatmap_enabled && (size_t)layer_idx < ctx.layer_heatmap_enabled->size()) {
        (*ctx.layer_heatmap_enabled)[(size_t)layer_idx] = true;
    }
    if (ctx.layer_heatmap_algo && (size_t)layer_idx < ctx.layer_heatmap_algo->size()) {
        (*ctx.layer_heatmap_algo)[(size_t)layer_idx] = kAggregateMedianChoropleth;
    }
    if (ctx.enqueue_hydration) ctx.enqueue_hydration((size_t)layer_idx, true);
    if (ctx.layer_heatmap_state_changed) *ctx.layer_heatmap_state_changed = true;
}

void setCategoryVisible(LayerUiSharedContext& ctx, int parcel_layer_idx, LayerDef::Category cat, bool enabled) {
    if (!ctx.layers) return;
    bool heatmap_changed = false;
    for (size_t i = 0; i < ctx.layers->size(); ++i) {
        LayerDef& layer = (*ctx.layers)[i];
        if (enabled && layer.scale == "parcel" && !layer.region.empty() && (int)i != parcel_layer_idx) continue;
        if (layer.category == cat && !hiddenParcelParameterLayer(ctx, parcel_layer_idx, i)) {
            layer.enabled = enabled;
            if (!enabled && layer.scale == "parcel" && ctx.layer_heatmap_enabled && i < ctx.layer_heatmap_enabled->size()) {
                (*ctx.layer_heatmap_enabled)[i] = false;
                heatmap_changed = true;
            }
        }
    }
    if (!enabled && cat == LayerDef::Category::Housing) {
        for (size_t i = 0; i < ctx.layers->size(); ++i) {
            LayerDef& layer = (*ctx.layers)[i];
            if (layer.category != LayerDef::Category::Housing || !hiddenParcelParameterLayer(ctx, parcel_layer_idx, i)) continue;
            layer.enabled = false;
            if (ctx.layer_heatmap_enabled && i < ctx.layer_heatmap_enabled->size()) {
                (*ctx.layer_heatmap_enabled)[i] = false;
                heatmap_changed = true;
            }
        }
        if (ctx.parcel_parameter_mode) *ctx.parcel_parameter_mode = 0;
    }
    if (heatmap_changed && ctx.layer_heatmap_state_changed) *ctx.layer_heatmap_state_changed = true;
}

bool downloadOrUpdateLayerVersioned(const LayerActionContext& ctx, bool local_layer_exists) {
    if (!ctx.shared || !ctx.layer || !ctx.shared->data_library_status_msg) return false;
    LayerUiSharedContext& shared = *ctx.shared;
    VersionedDownloadResult vd = downloadOrImportLayer(*ctx.layer, ctx.local_layer_path, shared.root);
    if (vd.ok) {
        *shared.data_library_status_msg =
            (vd.not_modified ? "Checked " : "Downloaded/updated ") + ctx.layer->file + " (" + vd.message + ")";
        setFreshness(shared, ctx.idx, FreshnessState::UpToDate, vd.message);
        if (shared.mark_local_layer_exists) shared.mark_local_layer_exists(ctx.idx, true);
        if (shared.enqueue_hydration) shared.enqueue_hydration(ctx.idx, true);
        return true;
    }

    *shared.data_library_status_msg = "Update failed for " + ctx.layer->file + ": " + vd.message;
    setFreshness(shared, ctx.idx, FreshnessState::Error, vd.message);
    return false;
}

bool checkLayerUpdateVersioned(const LayerActionContext& ctx) {
    if (!ctx.shared || !ctx.layer || !ctx.shared->data_library_status_msg) return false;
    const std::string freshness_url = ctx.layer->source_url.empty() ? ctx.layer->import_url : ctx.layer->source_url;
    FreshnessCheckResult cr = checkUrlFreshnessVersioned(
        freshness_url,
        ctx.local_layer_path,
        ctx.shared->root / "data" / "versions");
    setFreshness(*ctx.shared, ctx.idx, cr.state, cr.message);
    *ctx.shared->data_library_status_msg = "Checked " + ctx.layer->file + ": " + cr.message;
    return true;
}

bool deleteLocalLayerFile(const LayerActionContext& ctx) {
    if (!ctx.shared || !ctx.layer || !ctx.shared->data_library_status_msg) return false;
    LayerUiSharedContext& shared = *ctx.shared;
    std::error_code rm_ec;
    const bool removed = fs::remove(ctx.local_layer_path, rm_ec);
    if (!removed && fs::exists(ctx.local_layer_path)) {
        *shared.data_library_status_msg = "Failed to delete " + ctx.layer->file + ": " + rm_ec.message();
        return false;
    }

    const bool can_track_update = !ctx.layer->source_url.empty() || layerHasImportSource(*ctx.layer);
    ctx.layer->enabled = false;
    releaseContainerStorage(ctx.layer->features);
    if (shared.layer_spatial && ctx.idx < shared.layer_spatial->size()) {
        (*shared.layer_spatial)[ctx.idx] = LayerSpatialIndex{};
    }
    trimProcessHeap();
    if (shared.status_mutex && shared.layer_states) {
        std::lock_guard<std::mutex> lk_status(*shared.status_mutex);
        if (ctx.idx < shared.layer_states->size()) {
            (*shared.layer_states)[ctx.idx].status = LayerPipelineStatus::Queued;
            (*shared.layer_states)[ctx.idx].feature_count = 0;
            (*shared.layer_states)[ctx.idx].error.clear();
        }
    }
    if (shared.mark_local_layer_exists) shared.mark_local_layer_exists(ctx.idx, false);
    setFreshness(shared, ctx.idx,
        can_track_update ? FreshnessState::Unknown : FreshnessState::NotTrackable,
        can_track_update ? "not downloaded" : "no source URL/import source");
    *shared.data_library_status_msg = "Deleted local layer file: " + ctx.layer->file;
    return true;
}
