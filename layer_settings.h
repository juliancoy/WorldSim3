#pragma once

#include "layer_ui_contexts.h"

#include <filesystem>

struct LayerSettingsPopupContext {
    LayerUiSharedContext* shared = nullptr;
    int* active_hover_layer_idx = nullptr;
    int* active_click_layer_idx = nullptr;
    std::filesystem::path local_layer_path;
    size_t idx = 0;
    LayerDef* layer = nullptr;
    bool local_layer_exists = false;
    int zoom = 0;
};

void drawLayerDisplaySettingsPopup(LayerSettingsPopupContext& ctx);
