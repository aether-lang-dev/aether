// A texture at binding 0 (t0 with its sampler s0) scaled by a tint uniform at
// binding 1 (b1).
Texture2D tex : register(t0);
SamplerState smp : register(s0);
cbuffer Tint : register(b1) {
    float4 tint;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return tex.Sample(smp, uv) * tint;
}
