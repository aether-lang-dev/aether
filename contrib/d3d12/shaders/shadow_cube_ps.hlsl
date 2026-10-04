// A cube target's depth through a comparison sampler (#2412): whether the
// depth in the pushed direction (xyz) passes `reference op texel`, the
// reference in w, written to red. t0 with its comparison sampler s0.
TextureCube<float> shadowCube : register(t0);
SamplerComparisonState smp : register(s0);
cbuffer Push : register(b0, space1) {
    float4 dir;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return float4(shadowCube.SampleCmpLevelZero(smp, dir.xyz, dir.w), 0.0, 0.0, 1.0);
}
