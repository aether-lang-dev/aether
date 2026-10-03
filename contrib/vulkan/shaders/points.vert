#version 450

// The built-in layout drawn as points one pixel wide (#2398): Vulkan reads
// a point's size from gl_PointSize, which a point-drawing shader must write.
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    gl_PointSize = 1.0;
    fragColor = inColor;
}
