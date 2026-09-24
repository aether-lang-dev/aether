// A column-major mat4 pushed per draw (root constants at b0, space1): the
// same sixteen floats, in the same order, as contrib.vulkan's transform.vert
// takes as its push block.
cbuffer Push : register(b0, space1) {
    column_major float4x4 mvp;
};

struct VSIn  { float2 pos : TEXCOORD0; float3 col : TEXCOORD1; };
struct VSOut { float4 pos : SV_Position; float3 col : COLOR0; };

VSOut main(VSIn i) {
    VSOut o;
    o.pos = mul(mvp, float4(i.pos, 0.0, 1.0));
    o.col = i.col;
    return o;
}
