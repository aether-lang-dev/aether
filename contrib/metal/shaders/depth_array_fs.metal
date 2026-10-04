// A layered target's depth read as an array (#2412): the stored depth of the
// layer pushed per draw ([[buffer(8)]]), written to red so a float target
// reads it back exactly. The depth is [[texture(0)]] with [[sampler(0)]].
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

fragment float4 depth_array_fragment(FragmentIn in [[stage_in]],
                                     depth2d_array<float> depthLayers [[texture(0)]],
                                     sampler smp [[sampler(0)]],
                                     constant Push& push [[buffer(8)]]) {
    return float4(depthLayers.sample(smp, in.uv, uint(push.layer)), 0.0, 0.0, 1.0);
}
