#include "map_viewport.h"

#include "animation.h"
#include "app_utils.h"
#include "geo.h"
#include "map_render_utils.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace {
double cameraSmoothstep(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double wrapWorldX(double x, int math_zoom) {
    const double period = 256.0 * (double)(1u << math_zoom);
    x = std::fmod(x, period);
    if (x < 0.0) x += period;
    return x;
}

ImVec2 targetWorldPxNearCenter(const ImVec2& lonlat, const MapViewportFrame& frame) {
    ImVec2 world = lonLatToWorldPx(lonlat.x, lonlat.y, frame.math_zoom);
    const double period = 256.0 * (double)(1u << frame.math_zoom);
    double dx = (double)world.x - (double)frame.center_world.x;
    if (dx > period * 0.5) world.x -= (float)period;
    else if (dx < -period * 0.5) world.x += (float)period;
    return world;
}

double normalizeLongitude(double lon) {
    lon = std::fmod(lon + 180.0, 360.0);
    if (lon < 0.0) lon += 360.0;
    return lon - 180.0;
}

ImVec2 lonLatAtScreenPoint(const MapViewportFrame& frame, const ImVec2& screen) {
    ImVec2 world = ImVec2(
        frame.center_world.x + (float)((screen.x - (frame.origin.x + frame.size.x * 0.5f)) / frame.zoom_scale),
        frame.center_world.y + (float)((screen.y - (frame.origin.y + frame.size.y * 0.5f)) / frame.zoom_scale));
    world.x = (float)wrapWorldX((double)world.x, frame.math_zoom);
    return worldPxToLonLat(world, frame.math_zoom);
}

void setCenterFromWorld(MapViewportContext const& ctx, MapViewportFrame& frame, const ImVec2& center_world) {
    frame.center_world = center_world;
    frame.center_world.x = (float)wrapWorldX((double)frame.center_world.x, frame.math_zoom);
    const ImVec2 ll = worldPxToLonLat(frame.center_world, frame.math_zoom);
    *ctx.center_lon = normalizeLongitude(ll.x);
    *ctx.center_lat = std::clamp((double)ll.y, -85.0, 85.0);
}

void startAnimatedZoomToScreenPoint(
    const MapViewportContext& ctx,
    const MapViewportFrame& frame,
    double now_s,
    const ImVec2& screen,
    const ImVec2& target_ll,
    double target_zoom,
    double duration_s) {
    if (!ctx.camera_animation) return;
    const ImVec2 center_screen(
        frame.origin.x + frame.size.x * 0.5f,
        frame.origin.y + frame.size.y * 0.5f);
    startScreenSpaceZoomToPointAnimation(
        *ctx.camera_animation,
        now_s,
        *ctx.center_lon,
        *ctx.center_lat,
        *ctx.zoom,
        target_ll.x,
        target_ll.y,
        target_zoom,
        screen.x - center_screen.x,
        screen.y - center_screen.y,
        duration_s);
}

void startAnimatedZoomAtCenter(
    const MapViewportContext& ctx,
    double now_s,
    double target_zoom,
    double duration_s) {
    if (!ctx.camera_animation) {
        *ctx.zoom = target_zoom;
        return;
    }
    startCameraZoomAnimation(
        *ctx.camera_animation,
        now_s,
        *ctx.center_lon,
        *ctx.center_lat,
        *ctx.zoom,
        *ctx.center_lon,
        *ctx.center_lat,
        target_zoom,
        duration_s,
        CameraAnimationCurve::EaseOutCubic);
}

bool tickScreenSpaceZoomToPointAnimation(
    CameraAnimationState& state,
    double now_s,
    int max_internal_math_zoom,
    double& center_lon,
    double& center_lat,
    double& zoom) {
    if (!state.active) return false;
    const double t = state.duration_s <= 0.0
        ? 1.0
        : std::clamp((now_s - state.start_time_s) / state.duration_s, 0.0, 1.0);
    const double e = cameraSmoothstep(t);
    zoom = state.start_zoom + (state.target_zoom - state.start_zoom) * e;

    const int math_zoom = std::min((int)std::floor(zoom), max_internal_math_zoom);
    const double zoom_scale = std::exp2(zoom - math_zoom);
    ImVec2 target_world = lonLatToWorldPx(state.target_lon, state.target_lat, math_zoom);
    const ImVec2 remaining_screen_delta(
        (float)(state.start_screen_dx * (1.0 - e)),
        (float)(state.start_screen_dy * (1.0 - e)));
    ImVec2 center_world(
        target_world.x - (float)(remaining_screen_delta.x / zoom_scale),
        target_world.y - (float)(remaining_screen_delta.y / zoom_scale));
    center_world.x = (float)wrapWorldX((double)center_world.x, math_zoom);
    const ImVec2 center_ll = worldPxToLonLat(center_world, math_zoom);
    center_lon = center_ll.x;
    center_lat = std::clamp((double)center_ll.y, -85.0, 85.0);

    if (t >= 1.0) {
        center_lon = state.target_lon;
        center_lat = state.target_lat;
        zoom = state.target_zoom;
        state.active = false;
    }
    return true;
}

int pressedAltZoomFactor() {
    for (int factor = 1; factor <= 9; ++factor) {
        const ImGuiKey digit_key = (ImGuiKey)((int)ImGuiKey_0 + factor);
        const ImGuiKey keypad_key = (ImGuiKey)((int)ImGuiKey_Keypad0 + factor);
        if (ImGui::IsKeyPressed(digit_key, false) || ImGui::IsKeyPressed(keypad_key, false)) {
            return factor;
        }
    }
    return 0;
}

double zoomDeltaForFactor(int factor, double multiplier, double direction) {
    const double scale = std::clamp((double)factor * std::clamp(multiplier, 0.25, 16.0), 1.0, 144.0);
    return direction * std::log2(scale);
}

}

void drawMapTargetIndicator(TargetIndicatorState& state, const MapViewportFrame& frame, double now_s) {
    if (!state.active || !frame.draw) return;
    const double t = targetIndicatorProgress(state, now_s);
    if (t >= 1.0) {
        finishTargetIndicator(state);
        return;
    }

    const ImVec2 screen = frame.projectWorld(targetWorldPxNearCenter(ImVec2((float)state.lon, (float)state.lat), frame));
    if (screen.x < frame.origin.x - 80.0f || screen.y < frame.origin.y - 80.0f ||
        screen.x > frame.origin.x + frame.size.x + 80.0f ||
        screen.y > frame.origin.y + frame.size.y + 80.0f) {
        return;
    }

    const float eased = (float)(1.0 - std::pow(1.0 - t, 3.0));
    const float radius = 46.0f + (10.0f - 46.0f) * eased;
    const float alpha = (float)std::clamp(1.0 - t * 0.82, 0.0, 1.0);
    const ImU32 ring = IM_COL32(255, 218, 82, (int)std::lround(235.0f * alpha));
    const ImU32 core = IM_COL32(255, 255, 255, (int)std::lround(210.0f * alpha));
    frame.draw->AddCircle(screen, radius, ring, 48, 2.4f);
    frame.draw->AddCircle(screen, std::max(5.0f, radius * 0.34f), ring, 40, 1.6f);
    frame.draw->AddLine(ImVec2(screen.x - radius - 5.0f, screen.y), ImVec2(screen.x - radius * 0.44f, screen.y), ring, 1.5f);
    frame.draw->AddLine(ImVec2(screen.x + radius * 0.44f, screen.y), ImVec2(screen.x + radius + 5.0f, screen.y), ring, 1.5f);
    frame.draw->AddLine(ImVec2(screen.x, screen.y - radius - 5.0f), ImVec2(screen.x, screen.y - radius * 0.44f), ring, 1.5f);
    frame.draw->AddLine(ImVec2(screen.x, screen.y + radius * 0.44f), ImVec2(screen.x, screen.y + radius + 5.0f), ring, 1.5f);
    frame.draw->AddCircleFilled(screen, 2.5f, core, 16);
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
    const double clamped_zoom_step = std::clamp(ctx.zoom_step, 0.05, 4.0);
    const double clamped_scroll_zoom_distance = std::clamp(ctx.scroll_zoom_distance, 0.01, 4.0);
    const double clamped_scroll_zoom_duration = std::clamp(ctx.scroll_zoom_duration_s, 0.0, 1.5);
    const double clamped_alt_zoom_number_multiplier = std::clamp(ctx.alt_zoom_number_multiplier, 0.25, 16.0);
    static int pending_alt_zoom_factor = 0;

    frame.origin = ImGui::GetCursorScreenPos();
    frame.size = ImGui::GetContentRegionAvail();
    frame.draw = ImGui::GetWindowDrawList();
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("map_canvas_input", frame.size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    frame.hovered = ImGui::IsItemHovered(
        ImGuiHoveredFlags_AllowWhenBlockedByPopup |
        ImGuiHoveredFlags_AllowWhenOverlappedByWindow);
    frame.active = ImGui::IsItemActive();

    frame.draw->AddRectFilled(
        frame.origin,
        ImVec2(frame.origin.x + frame.size.x, frame.origin.y + frame.size.y),
        ctx.dark_mode ? IM_COL32(8, 13, 18, 255) : IM_COL32(242, 246, 250, 255));

    frame.math_zoom = std::min((int)std::floor(*ctx.zoom), ctx.max_internal_math_zoom);
    frame.zoom_scale = std::exp2(*ctx.zoom - frame.math_zoom);
    frame.center_world = lonLatToWorldPx(*ctx.center_lon, *ctx.center_lat, frame.math_zoom);
    frame.center_world.x = (float)wrapWorldX((double)frame.center_world.x, frame.math_zoom);
    const double now_s = ImGui::GetTime();
    const bool camera_ticked =
        ctx.camera_animation &&
        (ctx.camera_animation->curve == CameraAnimationCurve::ScreenSpaceZoomToPoint
            ? tickScreenSpaceZoomToPointAnimation(
                *ctx.camera_animation,
                now_s,
                ctx.max_internal_math_zoom,
                *ctx.center_lon,
                *ctx.center_lat,
                *ctx.zoom)
            : tickCameraAnimation(*ctx.camera_animation, now_s, *ctx.center_lon, *ctx.center_lat, *ctx.zoom));
    if (camera_ticked) {
        *ctx.zoom = std::clamp(*ctx.zoom, (double)ctx.min_zoom, (double)ctx.max_zoom);
        frame.math_zoom = std::min((int)std::floor(*ctx.zoom), ctx.max_internal_math_zoom);
        frame.zoom_scale = std::exp2(*ctx.zoom - frame.math_zoom);
        frame.center_world = lonLatToWorldPx(*ctx.center_lon, *ctx.center_lat, frame.math_zoom);
        frame.center_world.x = (float)wrapWorldX((double)frame.center_world.x, frame.math_zoom);
    }

    if (frame.hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const double zoom_delta = std::clamp((double)wheel, -4.0, 4.0) * clamped_scroll_zoom_distance;
            const double zoom_base =
                ctx.smooth_scroll_zoom && ctx.camera_animation && ctx.camera_animation->active
                    ? ctx.camera_animation->target_zoom
                    : *ctx.zoom;
            const double next_zoom = std::clamp(
                zoom_base + zoom_delta,
                (double)ctx.min_zoom,
                (double)ctx.max_zoom);
            if (next_zoom != *ctx.zoom) {
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                const double start_lon = *ctx.center_lon;
                const double start_lat = *ctx.center_lat;
                const double start_zoom = *ctx.zoom;
                ImVec2 ll = lonLatAtScreenPoint(frame, mouse);
                const int target_math_zoom = std::min((int)std::floor(next_zoom), ctx.max_internal_math_zoom);
                const double target_zoom_scale = std::exp2(next_zoom - target_math_zoom);
                ImVec2 mouse_world_new = lonLatToWorldPx(ll.x, ll.y, target_math_zoom);
                ImVec2 target_center_world = ImVec2(
                    mouse_world_new.x - (float)((mouse.x - (frame.origin.x + frame.size.x * 0.5f)) / target_zoom_scale),
                    mouse_world_new.y - (float)((mouse.y - (frame.origin.y + frame.size.y * 0.5f)) / target_zoom_scale));
                target_center_world.x = (float)wrapWorldX((double)target_center_world.x, target_math_zoom);
                ImVec2 target_center_ll = worldPxToLonLat(target_center_world, target_math_zoom);
                const double target_lon = target_center_ll.x;
                const double target_lat = std::clamp((double)target_center_ll.y, -85.0, 85.0);
                if (ctx.smooth_scroll_zoom && ctx.camera_animation) {
                    startCameraZoomAnimation(
                        *ctx.camera_animation,
                        now_s,
                        start_lon,
                        start_lat,
                        start_zoom,
                        target_lon,
                        target_lat,
                        next_zoom,
                        clamped_scroll_zoom_duration,
                        CameraAnimationCurve::EaseOutCubic);
                } else {
                    if (ctx.camera_animation) cancelCameraAnimation(*ctx.camera_animation);
                    *ctx.zoom = next_zoom;
                    frame.math_zoom = target_math_zoom;
                    frame.zoom_scale = target_zoom_scale;
                    frame.center_world = target_center_world;
                    *ctx.center_lon = target_lon;
                    *ctx.center_lat = target_lat;
                }
            }
        }

        if (frame.active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            if (ctx.camera_animation) cancelCameraAnimation(*ctx.camera_animation);
            ImVec2 d = ImGui::GetIO().MouseDelta;
            frame.center_world.x -= (float)(d.x / frame.zoom_scale);
            frame.center_world.y -= (float)(d.y / frame.zoom_scale);
            setCenterFromWorld(ctx, frame, frame.center_world);
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && io.KeyAlt) {
            const int factor = pressedAltZoomFactor();
            if (factor > 0) pending_alt_zoom_factor = factor;
        }
        if (pending_alt_zoom_factor > 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            pending_alt_zoom_factor = 0;
        }
        if (pending_alt_zoom_factor > 0) {
            const ImVec2 pad(10.0f, 8.0f);
            const ImVec2 text_pos(frame.origin.x + 12.0f, frame.origin.y + 12.0f);
            const double effective_factor = (double)pending_alt_zoom_factor * clamped_alt_zoom_number_multiplier;
            std::ostringstream label_os;
            label_os << "Alt zoom " << std::setprecision(3) << effective_factor << "x";
            const std::string label = label_os.str();
            const ImVec2 text_size = ImGui::CalcTextSize(label.c_str());
            frame.draw->AddRectFilled(
                ImVec2(text_pos.x - pad.x, text_pos.y - pad.y),
                ImVec2(text_pos.x + text_size.x + pad.x, text_pos.y + text_size.y + pad.y),
                ctx.dark_mode ? IM_COL32(20, 27, 34, 220) : IM_COL32(255, 255, 255, 230),
                5.0f);
            frame.draw->AddRect(
                ImVec2(text_pos.x - pad.x, text_pos.y - pad.y),
                ImVec2(text_pos.x + text_size.x + pad.x, text_pos.y + text_size.y + pad.y),
                ctx.dark_mode ? IM_COL32(110, 130, 150, 190) : IM_COL32(80, 96, 112, 165),
                5.0f);
            frame.draw->AddText(text_pos, ctx.dark_mode ? IM_COL32(238, 244, 250, 255) : IM_COL32(28, 35, 42, 255), label.c_str());
        }
        const bool keyboard_navigation_allowed = !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt;
        if (keyboard_navigation_allowed) {
            const double key_zoom_step = std::max(0.25, clamped_zoom_step);
            const bool zoom_in_key =
                ImGui::IsKeyPressed(ImGuiKey_Equal, true) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, true);
            const bool zoom_out_key =
                ImGui::IsKeyPressed(ImGuiKey_Minus, true) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, true);
            if (zoom_in_key || zoom_out_key) {
                const double direction = zoom_in_key ? 1.0 : -1.0;
                const double target_zoom = std::clamp(
                    *ctx.zoom + direction * key_zoom_step,
                    (double)ctx.min_zoom,
                    (double)ctx.max_zoom);
                if (target_zoom != *ctx.zoom) {
                    startAnimatedZoomAtCenter(ctx, now_s, target_zoom, 0.18);
                }
            }

            const bool home_key =
                ImGui::IsKeyPressed(ImGuiKey_Home, false) ||
                ImGui::IsKeyPressed(ImGuiKey_H, false);
            if (home_key && ctx.camera_animation) {
                startCameraZoomAnimation(
                    *ctx.camera_animation,
                    now_s,
                    *ctx.center_lon,
                    *ctx.center_lat,
                    *ctx.zoom,
                    -76.6122,
                    39.2904,
                    std::clamp(12.0, (double)ctx.min_zoom, (double)ctx.max_zoom),
                    0.42,
                    CameraAnimationCurve::ScaleNormalizedSmoothstep);
            }

            const bool left = ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_A);
            const bool right = ImGui::IsKeyDown(ImGuiKey_RightArrow) || ImGui::IsKeyDown(ImGuiKey_D);
            const bool up = ImGui::IsKeyDown(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_W);
            const bool down = ImGui::IsKeyDown(ImGuiKey_DownArrow) || ImGui::IsKeyDown(ImGuiKey_S);
            const double x_dir = (right ? 1.0 : 0.0) - (left ? 1.0 : 0.0);
            const double y_dir = (down ? 1.0 : 0.0) - (up ? 1.0 : 0.0);
            if (x_dir != 0.0 || y_dir != 0.0) {
                if (ctx.camera_animation) cancelCameraAnimation(*ctx.camera_animation);
                const double fast = io.KeyShift ? 2.4 : 1.0;
                const double dt = std::clamp((double)io.DeltaTime, 0.0, 1.0 / 30.0);
                const double pan_screen_px = std::max(180.0, std::min(frame.size.x, frame.size.y) * 0.80) * fast * dt;
                const double len = std::sqrt(x_dir * x_dir + y_dir * y_dir);
                frame.center_world.x += (float)((x_dir / len) * pan_screen_px / frame.zoom_scale);
                frame.center_world.y += (float)((y_dir / len) * pan_screen_px / frame.zoom_scale);
                setCenterFromWorld(ctx, frame, frame.center_world);
            }
        }
    }

    frame.center_world = lonLatToWorldPx(*ctx.center_lon, *ctx.center_lat, frame.math_zoom);
    frame.center_world.x = (float)wrapWorldX((double)frame.center_world.x, frame.math_zoom);
    const ImVec2 mouse_screen = ImGui::GetIO().MousePos;
    frame.mouse_ll = lonLatAtScreenPoint(frame, mouse_screen);

    static ImVec2 context_ll(0.0f, 0.0f);
    const ImGuiIO& io = ImGui::GetIO();
    if (frame.hovered && !io.WantTextInput && io.KeyAlt) {
        const int factor = pressedAltZoomFactor();
        if (factor > 0) pending_alt_zoom_factor = factor;
    }
    const bool double_left_click =
        frame.hovered &&
        pending_alt_zoom_factor == 0 &&
        !io.KeyCtrl &&
        !io.KeyAlt &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (double_left_click) {
        const double direction = io.KeyShift ? -1.0 : 1.0;
        const double target_zoom = std::clamp(
            *ctx.zoom + direction * std::max(1.0, clamped_zoom_step),
            (double)ctx.min_zoom,
            (double)ctx.max_zoom);
        if (target_zoom != *ctx.zoom) {
            startAnimatedZoomToScreenPoint(
                ctx,
                frame,
                now_s,
                mouse_screen,
                frame.mouse_ll,
                target_zoom,
                0.26);
        }
    }
    const bool alt_left_click =
        frame.hovered &&
        io.KeyAlt &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <= 36.0f;
    const bool alt_right_click =
        frame.hovered &&
        io.KeyAlt &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] <= 36.0f;
    const bool pending_left_click =
        frame.hovered &&
        pending_alt_zoom_factor > 0 &&
        !io.KeyCtrl &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <= 36.0f;
    const bool pending_right_click =
        frame.hovered &&
        pending_alt_zoom_factor > 0 &&
        !io.KeyCtrl &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] <= 36.0f;
    if (ctx.camera_animation && (alt_left_click || pending_left_click)) {
        const int factor = pending_alt_zoom_factor > 0 ? pending_alt_zoom_factor : 4;
        const double target_zoom = std::clamp(
            *ctx.zoom + (pending_alt_zoom_factor > 0
                ? zoomDeltaForFactor(factor, clamped_alt_zoom_number_multiplier, 1.0)
                : std::log2(4.0)),
            (double)ctx.min_zoom,
            (double)ctx.max_zoom);
        if (io.KeyCtrl && ctx.target_indicator) {
            startTargetIndicator(*ctx.target_indicator, now_s, frame.mouse_ll.x, frame.mouse_ll.y);
        }
        startAnimatedZoomToScreenPoint(
            ctx,
            frame,
            now_s,
            mouse_screen,
            frame.mouse_ll,
            target_zoom,
            0.45);
        pending_alt_zoom_factor = 0;
        frame.navigation_click_consumed = true;
    } else if (ctx.camera_animation && (alt_right_click || pending_right_click)) {
        const int factor = pending_alt_zoom_factor > 0 ? pending_alt_zoom_factor : 4;
        const double target_zoom = std::clamp(
            *ctx.zoom + (pending_alt_zoom_factor > 0
                ? zoomDeltaForFactor(factor, clamped_alt_zoom_number_multiplier, -1.0)
                : -std::log2(4.0)),
            (double)ctx.min_zoom,
            (double)ctx.max_zoom);
        startAnimatedZoomToScreenPoint(
            ctx,
            frame,
            now_s,
            mouse_screen,
            frame.mouse_ll,
            target_zoom,
            0.45);
        pending_alt_zoom_factor = 0;
        frame.navigation_click_consumed = true;
    }

    if (frame.hovered && pending_alt_zoom_factor == 0 && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
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
