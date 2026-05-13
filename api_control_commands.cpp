#include "api_control_commands.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <limits>

void applyApiControlCommands(ApiControlContext& ctx) {
    if (!ctx.zoom || !ctx.center_lon || !ctx.center_lat ||
        !ctx.api_zoom_cmd || !ctx.api_lon_cmd || !ctx.api_lat_cmd) {
        return;
    }

    int zc = ctx.api_zoom_cmd->exchange(-1, std::memory_order_relaxed);
    if (zc >= ctx.min_zoom && zc <= ctx.max_zoom) *ctx.zoom = zc;
    double lonc = ctx.api_lon_cmd->exchange(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
    double latc = ctx.api_lat_cmd->exchange(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
    if (!std::isnan(lonc)) *ctx.center_lon = lonc;
    if (!std::isnan(latc)) *ctx.center_lat = std::clamp(latc, -85.0, 85.0);

    if (ctx.layer_api) applyLayerApiCommands(*ctx.layer_api);

    if (!ctx.api_ui_cmd_seq || !ctx.api_ui_cmd_kind || !ctx.api_ui_cmd_x || !ctx.api_ui_cmd_y ||
        !ctx.api_ui_cmd_button || !ctx.api_ui_cmd_scroll_y || !ctx.api_ui_cmd_last_seq ||
        !ctx.api_ui_mouse_release_pending || !ctx.api_ui_mouse_release_button) {
        return;
    }

    const uint64_t ui_seq = ctx.api_ui_cmd_seq->load(std::memory_order_relaxed);
    ImGuiIO& io = ImGui::GetIO();
    if (*ctx.api_ui_mouse_release_pending) {
        io.AddMouseButtonEvent(std::clamp(*ctx.api_ui_mouse_release_button, 0, 4), false);
        *ctx.api_ui_mouse_release_pending = false;
    }
    if (ui_seq != *ctx.api_ui_cmd_last_seq) {
        *ctx.api_ui_cmd_last_seq = ui_seq;
        const int kind = ctx.api_ui_cmd_kind->load(std::memory_order_relaxed);
        const float x = (float)ctx.api_ui_cmd_x->load(std::memory_order_relaxed);
        const float y = (float)ctx.api_ui_cmd_y->load(std::memory_order_relaxed);
        const int button = std::clamp(ctx.api_ui_cmd_button->load(std::memory_order_relaxed), 0, 4);
        const float scroll_y = (float)ctx.api_ui_cmd_scroll_y->load(std::memory_order_relaxed);
        if (kind == 1) {
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(button, true);
            *ctx.api_ui_mouse_release_pending = true;
            *ctx.api_ui_mouse_release_button = button;
        } else if (kind == 2) {
            io.AddMousePosEvent(x, y);
        } else if (kind == 3) {
            io.AddMouseWheelEvent(0.0f, scroll_y);
        }
    }
}
