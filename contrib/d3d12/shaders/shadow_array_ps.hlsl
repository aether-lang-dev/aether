// A layered target's depth through a comparison sampler (#2412): the fraction
// of the footprint in the pushed layer whose depth passes `reference op
// texel`, written to red. The depth is t0 with its comparison sampler s0.
Texture2DArray<float> cascades : register(t0);
SamplerComparisonState smp : register(s0);
cbuffer Push : register(b0, space1) {
    float layer;
    float ref;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return float4(cascades.SampleCmpLevelZero(smp, float3(uv, layer), ref), 0.0, 0.0, 1.0);
}
