#version 450

// The colour in the draw's window of a dynamic uniform buffer (#2198): the
// same binding reads a different block for each draw.
layout(std140, binding = 0) uniform Colour { vec4 c; } colour;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = colour.c;
}
