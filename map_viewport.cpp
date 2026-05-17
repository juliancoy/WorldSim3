#include "map_viewport.h"

#include "app_utils.h"
#include "geo.h"
#include "map_render_utils.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace {
double wrapWorldX(double x, int math_zoom) {
    const double period = 256.0 * (double)(1u << math_zoom);
    x = std::fmod(x, period);
    if (x < 0.0) x += period;
    return x;
}
}

ImVec2 MapViewportFrame::projectWorld(const ImVec2& world_px) const {
    const float screen_cx = origin.x + size.x * 0.5f;
    const float screen_cy = origin.y + size.y * 0.5f;
    const float zsf = (float)zoom_scale;
    return ImVec2(
        screen_cx + (world_px.x - center_world.x) * zsf,
        screen_cy + (world_px.y - center_world.y) * zsf);
}

MapViewportFrame beginMapViewportCanvas(const MapViewportContext& ctx) {
    MapViewportFrame frame;
    if (!ctx.center_lon || !ctx.center_lat || !ctx.zoom) return frame;

    frame.origin = ImGui::GetCursorScreenPos();
    frame.size = ImGui::GetContentRegionAvail();
    frame.draw = ImGui::GetWindowDrawList();
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("map_canvas_input", frame.size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    frame.hovered = ImGui::IsItemHovered();
    frame.active = ImGui::IsItemActive();

    frame.draw->AddRectFilled(
        frame.origin,
        ImVec2(frame.origin.x + frame.size.x, frame.origin.y + frame.size.y),
        ctx.dark_mode ? IM_COL32(8, 13, 18, 255) : IM_COL32(242, 246, 250, 255));

    frame.math_zoom = std::min(*ctx.zoom, ctx.max_internal_math_zoom);
    frame.zoom_scale = std::ldexp(1.0, *ctx.zoom - frame.math_zoom);
    frame.center_world = lonLatToWorldPx(*ctx.center_lon, *ctx.center_lat, frame.math_zoom);
    frame.center_world.x = (float)wrapWorldX((double)frame.center_world.x, frame.math_zoom);

    if (frame.hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            int next_zoom = std::clamp(*ctx.zoom + (wheel > 0 ? 1 : -1), ctx.min_zoom, ctx.max_zoom);
            if (next_zoom != *ctx.zoom) {
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                ImVec2 mouse_world = ImVec2(
                    frame.center_world.x + (float)((mouse.x - (frame.origin.x + frame.size.x * 0.5f)) / frame.zoom_scale),
                    frame.center_world.y + (float)((mouse.y - (frame.origin.y + frame.size.y * 0.5f)) / frame.zoom_scale));
                mouse_world.x = (float)wrapWorldX((double)mouse_world.x, frame.math_zoom);
                ImVec2 ll = worldPxToLonLat(mouse_world, frame.math_zoom);
                *ctx.zoom = next_zoom;
                frame.math_zoom = std::min(*ctx.zoom, ctx.max_internal_math_zoom);
                frame.zoom_scale = std::ldexp(1.0, *ctx.zoom - frame.math_zoom);
                ImVec2 mouse_world_new = lonLatToWorldPx(ll.x, ll.y, frame.math_zoom);
                frame.center_world = ImVec2(
                    mouse_world_new.x - (float)((mouse.x - (frame.origin.x + frame.size.x * 0.5f)) / frame.zoom_scale),
                    mouse_world_new.y - (float)((mouse.y - (frame.origin.y + frame.size.y * 0.5f)) / frame.zoom_scale));
                frame.center_world.x = (float)wrapWorldX((double)frame.center_world.x, frame.math_zoom);
                ImVec2 cll = worldPxToLonLat(frame.center_world, frame.math_zoom);
                *ctx.center_lon = cll.x;
                *ctx.center_lat = std::clamp((double)cll.y, -85.0, 85.0);
            }
        }

        if (frame.active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            ImVec2 d = ImGui::GetIO().MouseDelta;
            frame.center_world.x -= (float)(d.x / frame.zoom_scale);
            frame.center_world.y -= (float)(d.y / frame.zoom_scale);
            frame.center_world.x = (float)wrapWorldX((double)frame.center_world.x, frame.math_zoom);
            ImVec2 ll = worldPxToLonLat(frame.center_world, frame.math_zoom);
            *ctx.center_lon = ll.x;
            *ctx.center_lat = std::clamp((double)ll.y, -85.0, 85.0);
        }
    }

    frame.center_world = lonLatToWorldPx(*ctx.center_lon, *ctx.center_lat, frame.math_zoom);
    frame.center_world.x = (float)wrapWorldX((double)frame.center_world.x, frame.math_zoom);
    const ImVec2 mouse_screen = ImGui::GetIO().MousePos;
    const ImVec2 mouse_world(
        frame.center_world.x + (float)((mouse_screen.x - (frame.origin.x + frame.size.x * 0.5f)) / frame.zoom_scale),
        frame.center_world.y + (float)((mouse_screen.y - (frame.origin.y + frame.size.y * 0.5f)) / frame.zoom_scale));
    frame.mouse_ll = worldPxToLonLat(mouse_world, frame.math_zoom);

    static ImVec2 context_ll(0.0f, 0.0f);
    if (frame.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        context_ll = frame.mouse_ll;
        ImGui::OpenPopup("map_context_menu");
    }
    if (ImGui::BeginPopup("map_context_menu")) {
        ImGui::Text("Lon: %.6f", context_ll.x);
        ImGui::Text("Lat: %.6f", context_ll.y);
        ImGui::Separator();
        if (ImGui::MenuItem("Open Google Maps Street View")) {
            std::ostringstream url;
            url << std::fixed << std::setprecision(7)
                << "https://www.google.com/maps/@?api=1&map_action=pano&viewpoint="
                << context_ll.y << "," << context_ll.x;
            openUrlInBrowser(url.str());
        }
        ImGui::EndPopup();
    }

    const double half_w_world = (frame.size.x * 0.5) / frame.zoom_scale;
    const double half_h_world = (frame.size.y * 0.5) / frame.zoom_scale;
    ImVec2 ll_a = worldPxToLonLat(ImVec2(frame.center_world.x - (float)half_w_world, frame.center_world.y - (float)half_h_world), frame.math_zoom);
    ImVec2 ll_b = worldPxToLonLat(ImVec2(frame.center_world.x + (float)half_w_world, frame.center_world.y + (float)half_h_world), frame.math_zoom);
    frame.view_min_lon = std::min(ll_a.x, ll_b.x);
    frame.view_max_lon = std::max(ll_a.x, ll_b.x);
    frame.view_min_lat = std::min(ll_a.y, ll_b.y);
    frame.view_max_lat = std::max(ll_a.y, ll_b.y);
    return frame;
}
