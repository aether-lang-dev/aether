// A target's depth through a comparison sampler (#2373): the fraction of the
// sampler's footprint whose depth passes `reference op texel`, the reference
// pushed per draw ([[buffer(8)]]), written to red so a float target reads it
// back exactly. The depth is [[texture(0)]] with its sampler [[sampler(0)]].
#include <metal_stdlib>
using namespace metal;

struct FragmentIn {
    float4 pos [[position]];
    float2 uv  [[user(uv)]];
};

fragment float4 shadow_fragment(FragmentIn in [[stage_in]],
                                depth2d<float> shadowMap [[texture(0)]],
                                sampler smp [[sampler(0)]],
                                constant float& ref [[buffer(8)]]) {
    return float4(shadowMap.sample_compare(smp, in.uv, ref), 0.0, 0.0, 1.0);
}
