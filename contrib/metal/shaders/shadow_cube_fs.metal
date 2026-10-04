// A cube target's depth through a comparison sampler (#2412): whether the
// depth in the pushed direction (xyz) passes `reference op texel`, the
// reference in w, written to red.
#include <metal_stdlib>
using namespace metal;

struct FragmentIn {
    float4 pos [[position]];
    float2 uv  [[user(uv)]];
};

fragment float4 shadow_cube_fragment(FragmentIn in [[stage_in]],
                                     depthcube<float> shadowCube [[texture(0)]],
                                     sampler smp [[sampler(0)]],
                                     constant float4& dir [[buffer(8)]]) {
    return float4(shadowCube.sample_compare(smp, dir.xyz, dir.w), 0.0, 0.0, 1.0);
}
