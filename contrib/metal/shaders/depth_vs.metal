// A float3 position (with its depth) and a float3 colour.
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 pos [[attribute(0)]];
    float3 col [[attribute(1)]];
};
struct VertexOut {
    float4 pos [[position]];
    float3 col [[user(col)]];
};

vertex VertexOut depth_vertex(VertexIn in [[stage_in]]) {
    VertexOut o;
    o.pos = float4(in.pos, 1.0);
    o.col = in.col;
    return o;
}
