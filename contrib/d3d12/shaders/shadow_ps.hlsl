// A target's depth through a comparison sampler (#2373): the fraction of the
// sampler's footprint whose depth passes `reference op texel`, the reference
// pushed per draw (b0, space1), written to red so a float target reads it
// back exactly. The depth is t0 with its comparison sampler s0.
Texture2D<float> shadowMap : register(t0);
SamplerComparisonState smp : register(s0);
cbuffer Push : register(b0, space1) {
    float ref;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return float4(shadowMap.SampleCmpLevelZero(smp, uv, ref), 0.0, 0.0, 1.0);
}
