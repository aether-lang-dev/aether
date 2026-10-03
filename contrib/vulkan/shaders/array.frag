#version 450

// A 2D array read at the layer pushed per draw (#2387).
layout(binding = 0) uniform sampler2DArray layers;

layout(push_constant) uniform Push { float layer; } push;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(layers, vec3(fragUV, push.layer));
}
