// A cube target's depth read as a cube (#2412): the stored depth in the
// direction pushed per draw (b0, space1, xyz), written to red. t0 with s0.
TextureCube<float> depthCube : register(t0);
SamplerState smp : register(s0);
cbuffer Push : register(b0, space1) {
    float4 dir;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return float4(depthCube.Sample(smp, dir.xyz), 0.0, 0.0, 1.0);
}
