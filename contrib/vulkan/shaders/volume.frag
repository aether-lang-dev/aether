#version 450

// A 3D texture (#2198): the quad's UV picks the texel in a slice, and the
// push constant picks the slice, 0 to 1 through the volume's depth.
layout(binding = 0) uniform sampler3D volume;
layout(push_constant) uniform Slice { float w; } slice;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(volume, vec3(fragUV, slice.w));
}
