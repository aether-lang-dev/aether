#version 450

// A target's depth through a comparison sampler (#2373): the fraction of the
// sampler's footprint whose depth passes `reference op texel`, the reference
// pushed per draw, written to red so a float target reads it back exactly.
layout(binding = 0) uniform sampler2DShadow shadowMap;

layout(push_constant) uniform Push { float ref; } push;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(texture(shadowMap, vec3(fragUV, push.ref)), 0.0, 0.0, 1.0);
}
