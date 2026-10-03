// A cube map read in the direction pushed per draw (#2387): [[texture(0)]]
// with [[sampler(0)]], the direction in [[buffer(8)]].
#include <metal_stdlib>
using namespace metal;

struct FragmentIn {
    float4 pos [[position]];
    float2 uv  [[user(uv)]];
};

fragment float4 cube_fragment(FragmentIn in [[stage_in]],
                              texturecube<float> cube [[texture(0)]],
                              sampler smp [[sampler(0)]],
                              constant float4& dir [[buffer(8)]]) {
    return cube.sample(smp, dir.xyz);
}
