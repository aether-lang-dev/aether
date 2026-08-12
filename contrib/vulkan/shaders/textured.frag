#version 450

layout(binding = 0) uniform sampler2D tex;
layout(binding = 1) uniform Tint { vec4 rgba; } tint;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(tex, fragUV) * tint.rgba;
}
