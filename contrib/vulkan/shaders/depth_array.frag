#version 450

// A layered target's depth read as an array (#2412): the stored depth of the
// layer pushed per draw, 0 near to 1 far, written to red so a float target
// reads it back exactly.
layout(binding = 0) uniform sampler2DArray depthLayers;

layout(push_constant) uniform Push { float layer; float ref; } push;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(texture(depthLayers, vec3(fragUV, push.layer)).r, 0.0, 0.0, 1.0);
}
