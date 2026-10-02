// A 3D texture (#2198) at binding 0 ([[texture(0)]] with [[sampler(0)]]):
// the quad's UV picks the texel in a slice, and the push constant
// ([[buffer(8)]]) picks the slice, 0 to 1 through the volume's depth.
#include <metal_stdlib>
using namespace metal;

struct FragmentIn {
    float4 pos [[position]];
    float2 uv  [[user(uv)]];
};

fragment float4 volume_fragment(FragmentIn in [[stage_in]],
                                texture3d<float> volume [[texture(0)]],
                                sampler smp [[sampler(0)]],
                                constant float& w [[buffer(8)]]) {
    return volume.sample(smp, float3(in.uv, w));
}
