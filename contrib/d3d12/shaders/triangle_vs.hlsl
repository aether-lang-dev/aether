// The built-in layout: float2 position at TEXCOORD0, float3 colour at
// TEXCOORD1, passed through.
struct VSIn  { float2 pos : TEXCOORD0; float3 col : TEXCOORD1; };
struct VSOut { float4 pos : SV_Position; float3 col : COLOR0; };

VSOut main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.col = i.col;
    return o;
}
