#version 450

// A cube map read in the direction pushed per draw (#2387): the face and the
// texel come from the direction, the same way in all three APIs.
layout(binding = 0) uniform samplerCube cube;

layout(push_constant) uniform Push { vec4 dir; } push;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(cube, push.dir.xyz);
}
