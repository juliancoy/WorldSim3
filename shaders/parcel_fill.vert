#version 450

layout(location = 0) in vec2 inLonLat;
layout(location = 1) in uint inFeatureRef;

layout(std430, set = 0, binding = 0) readonly buffer ParcelColors {
    uint packed_colors[];
};

layout(push_constant) uniform PushConstants {
    vec2 center_world;
    vec2 viewport_origin;
    vec2 viewport_size;
    vec2 framebuffer_size;
    float math_zoom;
    float zoom_scale;
} pc;

layout(location = 0) flat out vec4 outColor;

const float PI = 3.14159265358979323846;

vec2 lonLatToWorldPx(vec2 lonlat, float zoom) {
    float scale = 256.0 * exp2(zoom);
    float x = (lonlat.x + 180.0) / 360.0 * scale;
    float lat_rad = radians(clamp(lonlat.y, -85.0, 85.0));
    float y = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / PI) * 0.5 * scale;
    return vec2(x, y);
}

void main() {
    vec2 world = lonLatToWorldPx(inLonLat, pc.math_zoom);
    vec2 screen;
    screen.x = pc.viewport_origin.x + pc.viewport_size.x * 0.5 + (world.x - pc.center_world.x) * pc.zoom_scale;
    screen.y = pc.viewport_origin.y + pc.viewport_size.y * 0.5 + (world.y - pc.center_world.y) * pc.zoom_scale;

    vec2 clip;
    clip.x = (screen.x / pc.framebuffer_size.x) * 2.0 - 1.0;
    clip.y = 1.0 - (screen.y / pc.framebuffer_size.y) * 2.0;
    gl_Position = vec4(clip, 0.0, 1.0);

    outColor = unpackUnorm4x8(packed_colors[inFeatureRef]);
}
