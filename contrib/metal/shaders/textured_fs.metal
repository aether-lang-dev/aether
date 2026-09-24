// A texture at binding 0 ([[texture(0)]] with [[sampler(0)]]) scaled by a
// tint uniform at binding 1 ([[buffer(1)]]).
#include <metal_stdlib>
using namespace metal;

struct FragmentIn {
    float4 pos [[position]];
    float2 uv  [[user(uv)]];
};

fragment float4 textured_fragment(FragmentIn in [[stage_in]],
                                  texture2d<float> tex [[texture(0)]],
                                  sampler smp [[sampler(0)]],
                                  constant float4& tint [[buffer(1)]]) {
    return tex.sample(smp, in.uv) * tint;
}
