#version 450

// Position and colour, transformed by a matrix the caller pushes. Push
// constants are the cheapest way to get a per-draw transform to a shader:
// no descriptor set, no buffer, no update.
layout(push_constant) uniform Push {
    mat4 mvp;
} push;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = push.mvp * vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
}
