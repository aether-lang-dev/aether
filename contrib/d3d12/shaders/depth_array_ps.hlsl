// A layered target's depth read as an array (#2412): the stored depth of the
// layer pushed per draw (b0, space1), written to red so a float target reads
// it back exactly. The depth is t0 with its sampler s0.
Texture2DArray<float> depthLayers : register(t0);
SamplerState smp : register(s0);
cbuffer Push : register(b0, space1) {
    float layer;
    float ref;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return float4(depthLayers.Sample(smp, float3(uv, layer)), 0.0, 0.0, 1.0);
}
