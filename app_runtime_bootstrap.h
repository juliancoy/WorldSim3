#pragma once

#include "filters.h"
#include "layer_runtime.h"
#include "types.h"

#include <array>
#include <filesystem>
#include <mutex>
#include <vector>

const std::array<const char*, 24>& defaultParcelJurisdictionOptions();
void seedDefaultParcelJurisdictions(ParcelJurisdictionFilterState& state);
void initializeLayerStateFromPersistedArtifacts(
    const std::filesystem::path& root,
    const std::vector<LayerDef>& layers,
    int parcel_layer_idx,
    std::mutex& status_mutex,
    std::vector<LayerRuntimeState>& layer_states,
    size_t idx);
void initializeEnabledLayerStatesFromPersistedArtifacts(
    const std::filesystem::path& root,
    const std::vector<LayerDef>& layers,
    int parcel_layer_idx,
    std::mutex& status_mutex,
    std::vector<LayerRuntimeState>& layer_states);
size_t countReadyLayerStates(std::mutex& status_mutex, const std::vector<LayerRuntimeState>& layer_states);
