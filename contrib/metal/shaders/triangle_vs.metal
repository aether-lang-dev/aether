// The built-in layout: float2 position at attribute 0, float3 colour at
// attribute 1, passed through.
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 pos [[attribute(0)]];
    float3 col [[attribute(1)]];
};
struct VertexOut {
    float4 pos [[position]];
    float3 col [[user(col)]];
};

vertex VertexOut triangle_vertex(VertexIn in [[stage_in]]) {
    VertexOut o;
    o.pos = float4(in.pos, 0.0, 1.0);
    o.col = in.col;
    return o;
}
