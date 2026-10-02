// A shape drawn once per instance (#2198): its corners come from stream 0,
// the target's own vertices ([[buffer(16)]]), and each instance's offset and
// colour from stream 1 ([[buffer(17)]]), which advances once per instance.
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 pos    [[attribute(0)]];
    float2 offset [[attribute(1)]];
    float3 col    [[attribute(2)]];
};
struct VertexOut {
    float4 pos [[position]];
    float3 col [[user(col)]];
};

vertex VertexOut instanced_vertex(VertexIn in [[stage_in]]) {
    VertexOut o;
    o.pos = float4(in.pos + in.offset, 0.0, 1.0);
    o.col = in.col;
    return o;
}
