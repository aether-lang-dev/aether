// A shape drawn once per instance (#2198): its corners come from slot 0, the
// target's own vertices (TEXCOORD0), and each instance's offset and colour
// from slot 1, a stream that advances once per instance (TEXCOORD1, 2).
struct VSIn  { float2 pos : TEXCOORD0; float2 offset : TEXCOORD1; float3 col : TEXCOORD2; };
struct VSOut { float4 pos : SV_Position; float3 col : COLOR0; };

VSOut main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos + i.offset, 0.0, 1.0);
    o.col = i.col;
    return o;
}
