// A layered target's depth through a comparison sampler (#2412): the fraction
// of the footprint in the pushed layer whose depth passes `reference op
// texel`, written to red. [[texture(0)]] with its sampler [[sampler(0)]].
#include <metal_stdlib>
using namespace metal;

struct FragmentIn {
    float4 pos [[position]];
    float2 uv  [[user(uv)]];
};

struct Push {
    float layer;
    float ref;
};

fragment float4 shadow_array_fragment(FragmentIn in [[stage_in]],
                                      depth2d_array<float> cascades [[texture(0)]],
                                      sampler smp [[sampler(0)]],
                                      constant Push& push [[buffer(8)]]) {
    return float4(cascades.sample_compare(smp, in.uv, uint(push.layer), push.ref), 0.0, 0.0, 1.0);
}
