// A 2D array read at the layer pushed per draw (#2387).
Texture2DArray layers : register(t0);
SamplerState smp : register(s0);
cbuffer Push : register(b0, space1) {
    float layer;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return layers.Sample(smp, float3(uv, layer));
}
