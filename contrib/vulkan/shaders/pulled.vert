#version 450

// Vertices pulled from a storage buffer by index instead of fed through a
// vertex binding: what a draw of compute-generated geometry looks like.
layout(std430, binding = 0) readonly buffer Positions { vec2 p[]; } positions;

layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = vec4(positions.p[gl_VertexIndex], 0.0, 1.0);
    fragColor = vec3(1.0, 0.5, 0.0);
}
