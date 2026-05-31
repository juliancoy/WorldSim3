#pragma once

#include "imgui.h"

#include <cstddef>
#include <string>
#include <vector>

struct RoadLabelSelection {
    std::string label;
    int layer_idx = -1;
    size_t feature_idx = (size_t)-1;
    ImVec2 anchor_lonlat = ImVec2(0.0f, 0.0f);
    float angle_rad = 0.0f;
};

struct RoadLabelState {
    bool visible = true;
    size_t selected_idx = (size_t)-1;
    bool inspector_open_requested = false;
    std::vector<RoadLabelSelection> selections;
};
