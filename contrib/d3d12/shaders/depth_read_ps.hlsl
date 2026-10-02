// A target's depth read as a texture (#2198): the stored depth, 0 near to
// 1 far, written to red so a float target reads it back exactly. The depth
// is bound at binding 0 (t0 with its sampler s0).
Texture2D<float> depthTex : register(t0);
SamplerState smp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return float4(depthTex.Sample(smp, uv), 0.0, 0.0, 1.0);
}
