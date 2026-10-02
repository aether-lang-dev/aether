#version 450

// A target's depth read as a texture (#2198): the stored depth, 0 near to
// 1 far, written to red so a float target reads it back exactly.
layout(binding = 0) uniform sampler2D depthTex;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(texture(depthTex, fragUV).r, 0.0, 0.0, 1.0);
}
