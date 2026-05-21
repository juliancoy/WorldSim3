#version 450

layout(location = 0) flat in uint inFeatureRef;
layout(location = 0) out uint outFeatureRef;

void main() {
    outFeatureRef = inFeatureRef;
}
