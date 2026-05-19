#version 450

layout(location = 0) in vec2 inLocal;
layout(location = 1) flat in vec4 inColor;
layout(location = 2) flat in uint inGlyph;
layout(location = 0) out vec4 outColor;

bool glyphContains(vec2 p, uint glyph) {
    if (glyph == 1u) {
        return max(abs(p.x), abs(p.y)) <= 1.0;
    }
    if (glyph == 2u) {
        return abs(p.x) + abs(p.y) <= 1.0;
    }
    if (glyph == 3u) {
        vec2 a = vec2(0.0, -1.0);
        vec2 b = vec2(0.92, 0.82);
        vec2 c = vec2(-0.92, 0.82);
        float d1 = (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y);
        float d2 = (p.x - c.x) * (b.y - c.y) - (b.x - c.x) * (p.y - c.y);
        float d3 = (p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y);
        bool has_neg = (d1 < 0.0) || (d2 < 0.0) || (d3 < 0.0);
        bool has_pos = (d1 > 0.0) || (d2 > 0.0) || (d3 > 0.0);
        return !(has_neg && has_pos);
    }
    if (glyph == 4u) {
        return (abs(p.x) <= 0.26 && abs(p.y) <= 0.82) ||
               (abs(p.y) <= 0.26 && abs(p.x) <= 0.82);
    }
    if (glyph == 5u) {
        return abs(p.x - p.y) <= 0.24 || abs(p.x + p.y) <= 0.24;
    }
    if (glyph == 6u) {
        vec2 q = p;
        q.y += 0.18;
        bool circle = dot(q, q) <= 0.72;
        bool tip = p.y <= 0.05 && abs(p.x) <= (0.74 * (p.y + 1.0));
        return circle || tip;
    }
    return dot(p, p) <= 1.0;
}

void main() {
    if (inColor.a <= 0.0) discard;
    if (!glyphContains(inLocal, inGlyph)) discard;
    outColor = inColor;
}
