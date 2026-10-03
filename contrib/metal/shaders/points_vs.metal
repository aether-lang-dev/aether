// The built-in layout drawn as points one pixel wide (#2398): Metal reads a
// point's size from [[point_size]], which a point-drawing shader must write.
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 pos [[attribute(0)]];
    float3 col [[attribute(1)]];
};
struct VertexOut {
    float4 pos [[position]];
    float  size [[point_size]];
    float3 col [[user(col)]];
};

vertex VertexOut points_vertex(VertexIn in [[stage_in]]) {
    VertexOut o;
    o.pos = float4(in.pos, 0.0, 1.0);
    o.size = 1.0;
    o.col = in.col;
    return o;
}
