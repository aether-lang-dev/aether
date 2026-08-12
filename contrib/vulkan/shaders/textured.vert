#version 450

// A caller-described layout: position, normal and UV interleaved. The normal
// is not read here; it is present so the layout under test is a real mesh
// vertex rather than the built-in two-attribute one.
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec2 fragUV;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    fragUV = inUV;
}
