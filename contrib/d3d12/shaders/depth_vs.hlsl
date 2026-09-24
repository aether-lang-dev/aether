// A float3 position (with its depth) and a float3 colour.
struct VSIn  { float3 pos : TEXCOORD0; float3 col : TEXCOORD1; };
struct VSOut { float4 pos : SV_Position; float3 col : COLOR0; };

VSOut main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos, 1.0);
    o.col = i.col;
    return o;
}
