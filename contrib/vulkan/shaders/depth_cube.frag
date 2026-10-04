#version 450

// A cube target's depth read as a cube (#2412): the stored depth in the
// direction pushed per draw (xyz), written to red.
layout(binding = 0) uniform samplerCube depthCube;

layout(push_constant) uniform Push { vec4 dir; } push;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(texture(depthCube, push.dir.xyz).r, 0.0, 0.0, 1.0);
}
