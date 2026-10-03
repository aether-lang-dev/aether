// Writes `n` 32-bit words from word `at` of a buffer: the commands an
// indirect draw reads, set on the GPU (#2198). The five values are separate
// members because a cbuffer array would pad each one to 16 bytes, where the
// push constants are seven packed words.
RWStructuredBuffer<uint> words : register(u0);
cbuffer Push : register(b0, space1) {
    uint at;
    uint n;
    uint v0;
    uint v1;
    uint v2;
    uint v3;
    uint v4;
};

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint v[5] = { v0, v1, v2, v3, v4 };
    for (uint i = 0; i < n; i++) {
        words[at + i] = v[i];
    }
}
