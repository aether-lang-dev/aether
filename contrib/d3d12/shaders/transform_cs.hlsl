// out[i] = in[i] * in[i] * scale + offset + bias, and n[i] = n[i] * 3 + 7 in
// place: contrib.vulkan's transform.comp in HLSL, checked by the same test.
RWStructuredBuffer<float> src  : register(u0);
RWStructuredBuffer<float> dst  : register(u1);
RWStructuredBuffer<int>   ints : register(u2);
cbuffer Params : register(b3) {
    float bias;
};
cbuffer Push : register(b0, space1) {
    float scale;
    float offset;
    uint  count;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= count) return;
    float x = src[i];
    dst[i] = x * x * scale + offset + bias;
    ints[i] = ints[i] * 3 + 7;
}
