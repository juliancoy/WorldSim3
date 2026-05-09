#pragma once

#include "imgui.h"
#include "policy_panel.h"
#include "time_cube_panel.h"

#include <cstddef>

void drawMapOverlayPanelsPopup(
    ImVec2 origin,
    ImVec2 size,
    TimeCubePanelContext& time_cube_panel_ctx,
    PolicyPanelContext& policy_panel_ctx,
    size_t layer_count);
