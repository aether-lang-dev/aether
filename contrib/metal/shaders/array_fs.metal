// A 2D array read at the layer pushed per draw (#2387).
#include <metal_stdlib>
using namespace metal;

struct FragmentIn {
    float4 pos [[position]];
    float2 uv  [[user(uv)]];
};

fragment float4 array_fragment(FragmentIn in [[stage_in]],
                               texture2d_array<float> layers [[texture(0)]],
                               sampler smp [[sampler(0)]],
                               constant float& layer [[buffer(8)]]) {
    return layers.sample(smp, in.uv, uint(layer));
}
