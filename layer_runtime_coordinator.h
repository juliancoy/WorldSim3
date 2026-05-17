#pragma once

#include "layer_registry.h"
#include "layer_runtime.h"
#include "types.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct LayerApiCommandCoordinatorContext {
    std::vector<LayerDef>* layers = nullptr;
    LayerRegistry* layer_registry = nullptr;
    std::mutex* api_layer_mutex = nullptr;
    std::unordered_map<std::string, bool>* api_layer_enable_cmds = nullptr;
    std::unordered_map<std::string, bool>* api_layer_fill_cmds = nullptr;
    std::vector<std::string>* api_layer_download_cmds = nullptr;
    std::vector<bool>* layer_profile_dirty = nullptr;
    std::mutex* layer_fill_mutex = nullptr;
    std::vector<bool>* layer_fill_enabled = nullptr;
    bool* layer_fill_state_changed = nullptr;
    std::string* data_library_status_msg = nullptr;
    std::function<bool(size_t)> enqueue_layer_download_request;
};

struct LayerDependencyCoordinatorContext {
    const std::vector<LayerDef>* layers = nullptr;
    const std::vector<LayerRuntimeState>* layer_states = nullptr;
    std::mutex* status_mutex = nullptr;

    int parcel_layer_idx = -1;
    int real_property_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;

    bool filter_enabled = false;
    const char* filter_owner = nullptr;
    const char* filter_address = nullptr;
    const char* filter_zip = nullptr;

    std::function<void(size_t, bool)> enqueue_hydration;
};

void applyLayerApiCommands(LayerApiCommandCoordinatorContext& ctx);
void coordinateLayerHydrationDependencies(const LayerDependencyCoordinatorContext& ctx);
