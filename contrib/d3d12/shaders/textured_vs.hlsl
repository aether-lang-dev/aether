// position(2) + normal(3) + uv(2), interleaved: locations 0, 1 and 2.
struct VSIn  { float2 pos : TEXCOORD0; float3 normal : TEXCOORD1; float2 uv : TEXCOORD2; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.uv = i.uv;
    return o;
}
