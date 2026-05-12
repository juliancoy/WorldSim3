#pragma once

#include "layer_ui_contexts.h"

#include <filesystem>
#include <string_view>

struct LayerActionContext {
    LayerUiSharedContext* shared = nullptr;
    size_t idx = 0;
    LayerDef* layer = nullptr;
    std::filesystem::path local_layer_path;
};

int findLayerFile(const LayerUiSharedContext& ctx, std::string_view file);
bool hiddenParcelParameterLayer(const LayerUiSharedContext& ctx, int parcel_layer_idx, size_t idx);
void setParcelParameterMode(LayerUiSharedContext& ctx, int mode);
void activateParameterLayer(LayerUiSharedContext& ctx, int layer_idx);
void setCategoryVisible(LayerUiSharedContext& ctx, int parcel_layer_idx, LayerDef::Category cat, bool enabled);
bool downloadOrUpdateLayerVersioned(const LayerActionContext& ctx, bool local_layer_exists);
bool checkLayerUpdateVersioned(const LayerActionContext& ctx);
bool deleteLocalLayerFile(const LayerActionContext& ctx);
