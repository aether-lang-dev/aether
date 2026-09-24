// A triangle's three corners, scaled by a push constant, for pulled_vs.hlsl.
RWStructuredBuffer<float2> positions : register(u0);
cbuffer Push : register(b0, space1) {
    float scale;
};

static const float2 corners[3] = { float2(0.0, 1.0), float2(-1.0, -1.0), float2(1.0, -1.0) };

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    positions[id.x] = corners[id.x] * scale;
}
