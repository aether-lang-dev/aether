// A cube target's depth read as a cube (#2412): the stored depth in the
// direction pushed per draw ([[buffer(8)]], xyz), written to red.
#include <metal_stdlib>
using namespace metal;

struct FragmentIn {
    float4 pos [[position]];
    float2 uv  [[user(uv)]];
};

fragment float4 depth_cube_fragment(FragmentIn in [[stage_in]],
                                    depthcube<float> depthCube [[texture(0)]],
                                    sampler smp [[sampler(0)]],
                                    constant float4& dir [[buffer(8)]]) {
    return float4(depthCube.sample(smp, dir.xyz), 0.0, 0.0, 1.0);
}
