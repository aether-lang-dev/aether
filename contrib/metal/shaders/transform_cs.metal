// out[i] = in[i] * in[i] * scale + offset + bias, and n[i] = n[i] * 3 + 7 in
// place: contrib.vulkan's transform.comp in MSL, checked by the same test.
// Run with 64 threads a group (compute_set_group_size), as the GLSL declares.
#include <metal_stdlib>
using namespace metal;

struct Push {
    float scale;
    float offset;
    uint  count;
};

kernel void transform(device const float* src  [[buffer(0)]],
                      device float*       dst  [[buffer(1)]],
                      device int*         ints [[buffer(2)]],
                      constant float&     bias [[buffer(3)]],
                      constant Push&      push [[buffer(8)]],
                      uint i [[thread_position_in_grid]]) {
    if (i >= push.count) return;
    float x = src[i];
    dst[i] = x * x * push.scale + push.offset + bias;
    ints[i] = ints[i] * 3 + 7;
}
