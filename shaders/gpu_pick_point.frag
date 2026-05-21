#version 450

layout(location = 0) in vec2 inLocal;
layout(location = 1) flat in uint inFeatureRef;
layout(location = 0) out uint outFeatureRef;

void main() {
    if (dot(inLocal, inLocal) > 1.0) discard;
    outFeatureRef = inFeatureRef;
}
