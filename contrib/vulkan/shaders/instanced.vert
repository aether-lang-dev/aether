#version 450

// A shape drawn once per instance (#2198): its corners come from binding 0,
// the target's own vertices, and each instance's offset and colour from
// binding 1, a stream that advances once per instance.
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inOffset;
layout(location = 2) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = vec4(inPosition + inOffset, 0.0, 1.0);
    fragColor = inColor;
}
