#version 450

// Solid green, so a covered pixel is told from the clear colour.
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(0.0, 1.0, 0.0, 1.0);
}
