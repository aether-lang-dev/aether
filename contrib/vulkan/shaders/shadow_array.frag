#version 450

// A layered target's depth through a comparison sampler (#2412): the fraction
// of the footprint in the pushed layer whose depth passes `reference op
// texel`, written to red. What a cascade of shadow maps is read through.
layout(binding = 0) uniform sampler2DArrayShadow cascades;

layout(push_constant) uniform Push { float layer; float ref; } push;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(texture(cascades, vec4(fragUV, push.layer, push.ref)), 0.0, 0.0, 1.0);
}
