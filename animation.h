#pragma once

enum class CameraAnimationCurve {
    Smoothstep,
    EaseOutCubic,
    ScaleNormalizedSmoothstep,
    ScreenSpaceZoomToPoint
};

struct CameraAnimationState {
    bool active = false;
    double start_time_s = 0.0;
    double duration_s = 0.45;
    CameraAnimationCurve curve = CameraAnimationCurve::Smoothstep;
    double start_lon = 0.0;
    double start_lat = 0.0;
    double start_zoom = 0.0;
    double target_lon = 0.0;
    double target_lat = 0.0;
    double target_zoom = 0.0;
    double start_screen_dx = 0.0;
    double start_screen_dy = 0.0;
};

struct TargetIndicatorState {
    bool active = false;
    double start_time_s = 0.0;
    double duration_s = 0.70;
    double lon = 0.0;
    double lat = 0.0;
};

void startCameraZoomAnimation(
    CameraAnimationState& state,
    double now_s,
    double current_lon,
    double current_lat,
    double current_zoom,
    double target_lon,
    double target_lat,
    double target_zoom,
    double duration_s = 0.45,
    CameraAnimationCurve curve = CameraAnimationCurve::Smoothstep);

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
    double duration_s = 0.45);

bool tickCameraAnimation(
    CameraAnimationState& state,
    double now_s,
    double& center_lon,
    double& center_lat,
    double& zoom);

void cancelCameraAnimation(CameraAnimationState& state);

void startTargetIndicator(
    TargetIndicatorState& state,
    double now_s,
    double lon,
    double lat);

double targetIndicatorProgress(const TargetIndicatorState& state, double now_s);

void finishTargetIndicator(TargetIndicatorState& state);
