// Vertices pulled from a storage buffer by index: no vertex input at all.
RWStructuredBuffer<float2> positions : register(u0);

struct VSOut { float4 pos : SV_Position; float3 col : COLOR0; };

VSOut main(uint id : SV_VertexID) {
    VSOut o;
    o.pos = float4(positions[id], 0.0, 1.0);
    o.col = float3(1.0, 0.5, 0.0);
    return o;
}
