#pragma once

#include "types.h"

#include <cstddef>
#include <string>
#include <vector>

std::string mapTitleSourceLabelForLayer(const LayerDef& layer);
std::string mapTitleSourceDisplayName(const LayerDef& layer);
std::vector<size_t> mapTitleSourceLayerCandidates(const std::vector<LayerDef>& layers, int parcel_layer_idx);
size_t resolveMapTitleSourceLayerIndex(
    const std::vector<LayerDef>& layers,
    const std::string& selected_layer_file,
    int parcel_layer_idx);

