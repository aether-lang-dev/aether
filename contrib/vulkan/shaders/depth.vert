#version 450

// Position carries z, so the depth test has something to compare. The
// built-in layout is vec2; this one is declared by the caller through
// layout_attr, which is what #1508 added.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = vec4(inPosition, 1.0);
    fragColor = inColor;
}
