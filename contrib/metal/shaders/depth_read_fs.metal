// A target's depth read as a texture (#2198): the stored depth, 0 near to
// 1 far, written to red so a float target reads it back exactly. The depth
// is bound at binding 0 ([[texture(0)]] with [[sampler(0)]]).
#include <metal_stdlib>
using namespace metal;

struct FragmentIn {
    float4 pos [[position]];
    float2 uv  [[user(uv)]];
};

fragment float4 depth_read_fragment(FragmentIn in [[stage_in]],
                                    depth2d<float> depthTex [[texture(0)]],
                                    sampler smp [[sampler(0)]]) {
    return float4(depthTex.sample(smp, in.uv), 0.0, 0.0, 1.0);
}
