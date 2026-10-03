// A cube map read in the direction pushed per draw (#2387): t0 with its
// sampler s0, the direction in b0, space1.
TextureCube cube : register(t0);
SamplerState smp : register(s0);
cbuffer Push : register(b0, space1) {
    float4 dir;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return cube.Sample(smp, dir.xyz);
}
