#version 450

// A cube target's depth through a comparison sampler (#2412): whether the
// depth in the pushed direction (xyz) passes `reference op texel`, the
// reference pushed in w, written to red. A point light's shadow.
layout(binding = 0) uniform samplerCubeShadow shadowCube;

layout(push_constant) uniform Push { vec4 dir; } push;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(texture(shadowCube, push.dir), 0.0, 0.0, 1.0);
}
