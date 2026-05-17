#include "layer_runtime_coordinator.h"

#include "layer_import.h"

#include <mutex>

namespace {
bool layerRuntimeReady(const std::vector<LayerRuntimeState>& layer_states, std::mutex& status_mutex, int idx) {
    if (idx < 0 || (size_t)idx >= layer_states.size()) return false;
    std::lock_guard<std::mutex> lk(status_mutex);
    if ((size_t)idx >= layer_states.size()) return false;
    const LayerPipelineStatus st = layer_states[(size_t)idx].status;
    return st == LayerPipelineStatus::Hydrated ||
           st == LayerPipelineStatus::TriQueued ||
           st == LayerPipelineStatus::Triangulating ||
           st == LayerPipelineStatus::Ready;
}

int findLayerIndex(const LayerApiCommandCoordinatorContext& ctx, const std::string& file) {
    if (ctx.layer_registry) return ctx.layer_registry->findLayerByFile(file);
    if (!ctx.layers) return -1;
    for (size_t i = 0; i < ctx.layers->size(); ++i) {
        if ((*ctx.layers)[i].file == file) return (int)i;
    }
    return -1;
}
}

void applyLayerApiCommands(LayerApiCommandCoordinatorContext& ctx) {
    if (!ctx.layers || !ctx.api_layer_mutex || !ctx.api_layer_enable_cmds ||
        !ctx.api_layer_fill_cmds || !ctx.api_layer_download_cmds) {
        return;
    }

    std::lock_guard<std::mutex> lk(*ctx.api_layer_mutex);
    for (const auto& kv : *ctx.api_layer_enable_cmds) {
        const int idx = findLayerIndex(ctx, kv.first);
        if (idx < 0) continue;
        (*ctx.layers)[(size_t)idx].enabled = kv.second;
        if (ctx.layer_profile_dirty && (size_t)idx < ctx.layer_profile_dirty->size()) {
            (*ctx.layer_profile_dirty)[(size_t)idx] = true;
        }
    }
    ctx.api_layer_enable_cmds->clear();

    if (!ctx.api_layer_fill_cmds->empty() && ctx.layer_fill_mutex && ctx.layer_fill_enabled && ctx.layer_fill_state_changed) {
        std::lock_guard<std::mutex> lk_fill(*ctx.layer_fill_mutex);
        for (const auto& kv : *ctx.api_layer_fill_cmds) {
            const int idx = findLayerIndex(ctx, kv.first);
            if (idx < 0 || (size_t)idx >= ctx.layer_fill_enabled->size()) continue;
            if ((*ctx.layer_fill_enabled)[(size_t)idx] != kv.second) {
                (*ctx.layer_fill_enabled)[(size_t)idx] = kv.second;
                *ctx.layer_fill_state_changed = true;
            }
        }
        ctx.api_layer_fill_cmds->clear();
    }

    if (!ctx.api_layer_download_cmds->empty()) {
        for (const auto& file : *ctx.api_layer_download_cmds) {
            const int idx = findLayerIndex(ctx, file);
            if (idx < 0) {
                if (ctx.data_library_status_msg) {
                    *ctx.data_library_status_msg = "Download request ignored (unknown layer file): " + file;
                }
                continue;
            }
            LayerDef& layer = (*ctx.layers)[(size_t)idx];
            if (layer.source_url.empty() && !layerHasImportSource(layer)) {
                if (ctx.data_library_status_msg) {
                    *ctx.data_library_status_msg = "No source URL/import source for " + layer.file;
                }
                continue;
            }
            if (ctx.enqueue_layer_download_request) ctx.enqueue_layer_download_request((size_t)idx);
        }
        ctx.api_layer_download_cmds->clear();
    }
}

void coordinateLayerHydrationDependencies(const LayerDependencyCoordinatorContext& ctx) {
    if (!ctx.layers || !ctx.layer_states || !ctx.status_mutex || !ctx.enqueue_hydration) return;

    const bool vacant_layer_active =
        (ctx.vacant_notice_layer_idx >= 0 && (*ctx.layers)[(size_t)ctx.vacant_notice_layer_idx].enabled) ||
        (ctx.vacant_rehab_layer_idx >= 0 && (*ctx.layers)[(size_t)ctx.vacant_rehab_layer_idx].enabled);
    if (vacant_layer_active && ctx.parcel_layer_idx >= 0 &&
        !layerRuntimeReady(*ctx.layer_states, *ctx.status_mutex, ctx.parcel_layer_idx)) {
        ctx.enqueue_hydration((size_t)ctx.parcel_layer_idx, true);
    }

    if (ctx.real_property_layer_idx >= 0 && (size_t)ctx.real_property_layer_idx < ctx.layers->size()) {
        const bool filter_join_needed =
            ctx.filter_enabled ||
            (ctx.parcel_layer_idx >= 0 && (*ctx.layers)[(size_t)ctx.parcel_layer_idx].enabled) ||
            vacant_layer_active ||
            (ctx.filter_owner && ctx.filter_owner[0] != '\0') ||
            (ctx.filter_address && ctx.filter_address[0] != '\0') ||
            (ctx.filter_zip && ctx.filter_zip[0] != '\0');
        if (filter_join_needed &&
            !layerRuntimeReady(*ctx.layer_states, *ctx.status_mutex, ctx.real_property_layer_idx)) {
            ctx.enqueue_hydration((size_t)ctx.real_property_layer_idx, true);
        }
    }

    const bool tax_layer_active =
        (ctx.tax_lien_layer_idx >= 0 && (*ctx.layers)[(size_t)ctx.tax_lien_layer_idx].enabled) ||
        (ctx.tax_sale_layer_idx >= 0 && (*ctx.layers)[(size_t)ctx.tax_sale_layer_idx].enabled);
    if (tax_layer_active && ctx.parcel_layer_idx >= 0 &&
        !layerRuntimeReady(*ctx.layer_states, *ctx.status_mutex, ctx.parcel_layer_idx)) {
        ctx.enqueue_hydration((size_t)ctx.parcel_layer_idx, true);
    }

}
