// A 3D texture (#2198) at binding 0 (t0 with its sampler s0): the quad's UV
// picks the texel in a slice, and the push constant (b0, space1) picks the
// slice, 0 to 1 through the volume's depth.
Texture3D volume : register(t0);
SamplerState smp : register(s0);
cbuffer Slice : register(b0, space1) {
    float w;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return volume.Sample(smp, float3(uv, w));
}
