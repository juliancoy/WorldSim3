#include "animation.h"

#include <algorithm>
#include <cmath>

namespace {
double smoothstep(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double easeOutCubic(double t) {
    t = std::clamp(t, 0.0, 1.0);
    const double inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
}

double applyCurve(CameraAnimationCurve curve, double t) {
    switch (curve) {
        case CameraAnimationCurve::EaseOutCubic:
            return easeOutCubic(t);
        case CameraAnimationCurve::ScaleNormalizedSmoothstep:
            return smoothstep(t);
        case CameraAnimationCurve::Smoothstep:
        default:
            return smoothstep(t);
    }
}

double scaleNormalizedTranslationProgress(double zoom_progress, double zoom_delta) {
    zoom_progress = std::clamp(zoom_progress, 0.0, 1.0);
    if (std::abs(zoom_delta) < 1.0e-6) return zoom_progress;
    const double exponent = -std::log(2.0) * zoom_delta;
    const double denom = 1.0 - std::exp(exponent);
    if (std::abs(denom) < 1.0e-9) return zoom_progress;
    return std::clamp((1.0 - std::exp(exponent * zoom_progress)) / denom, 0.0, 1.0);
}

double shortestLongitudeDelta(double from_lon, double to_lon) {
    double delta = std::fmod(to_lon - from_lon + 540.0, 360.0) - 180.0;
    if (delta < -180.0) delta += 360.0;
    return delta;
}

double normalizeLongitude(double lon) {
    lon = std::fmod(lon + 180.0, 360.0);
    if (lon < 0.0) lon += 360.0;
    return lon - 180.0;
}
}

void startCameraZoomAnimation(
    CameraAnimationState& state,
    double now_s,
    double current_lon,
    double current_lat,
    double current_zoom,
    double target_lon,
    double target_lat,
    double target_zoom,
    double duration_s,
    CameraAnimationCurve curve) {
    state.active = true;
    state.start_time_s = now_s;
    state.duration_s = std::max(0.0, duration_s);
    state.curve = curve;
    state.start_lon = current_lon;
    state.start_lat = current_lat;
    state.start_zoom = current_zoom;
    state.target_lon = target_lon;
    state.target_lat = std::clamp(target_lat, -85.0, 85.0);
    state.target_zoom = target_zoom;
    state.start_screen_dx = 0.0;
    state.start_screen_dy = 0.0;
}

void startScreenSpaceZoomToPointAnimation(
    CameraAnimationState& state,
    double now_s,
    double current_lon,
    double current_lat,
    double current_zoom,
    double target_lon,
    double target_lat,
    double target_zoom,
    double start_screen_dx,
    double start_screen_dy,
    double duration_s) {
    startCameraZoomAnimation(
        state,
        now_s,
        current_lon,
        current_lat,
        current_zoom,
        target_lon,
        target_lat,
        target_zoom,
        duration_s,
        CameraAnimationCurve::ScreenSpaceZoomToPoint);
    state.start_screen_dx = start_screen_dx;
    state.start_screen_dy = start_screen_dy;
}

bool tickCameraAnimation(
    CameraAnimationState& state,
    double now_s,
    double& center_lon,
    double& center_lat,
    double& zoom) {
    if (!state.active) return false;
    const double t = state.duration_s <= 0.0
        ? 1.0
        : std::clamp((now_s - state.start_time_s) / state.duration_s, 0.0, 1.0);
    const double zoom_e = applyCurve(state.curve, t);
    const double center_e = state.curve == CameraAnimationCurve::ScaleNormalizedSmoothstep
        ? scaleNormalizedTranslationProgress(zoom_e, state.target_zoom - state.start_zoom)
        : zoom_e;
    center_lon = normalizeLongitude(state.start_lon + shortestLongitudeDelta(state.start_lon, state.target_lon) * center_e);
    center_lat = std::clamp(state.start_lat + (state.target_lat - state.start_lat) * center_e, -85.0, 85.0);
    zoom = state.start_zoom + (state.target_zoom - state.start_zoom) * zoom_e;
    if (t >= 1.0) {
        center_lon = normalizeLongitude(state.target_lon);
        center_lat = state.target_lat;
        zoom = state.target_zoom;
        state.active = false;
    }
    return true;
}

void cancelCameraAnimation(CameraAnimationState& state) {
    state.active = false;
}

void startTargetIndicator(
    TargetIndicatorState& state,
    double now_s,
    double lon,
    double lat) {
    state.active = true;
    state.start_time_s = now_s;
    state.duration_s = 0.70;
    state.lon = normalizeLongitude(lon);
    state.lat = std::clamp(lat, -85.0, 85.0);
}

double targetIndicatorProgress(const TargetIndicatorState& state, double now_s) {
    if (!state.active) return 1.0;
    if (state.duration_s <= 0.0) return 1.0;
    return std::clamp((now_s - state.start_time_s) / state.duration_s, 0.0, 1.0);
}

void finishTargetIndicator(TargetIndicatorState& state) {
    state.active = false;
}
