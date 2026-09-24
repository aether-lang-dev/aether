// position(2) + normal(3) + uv(2), interleaved: attributes 0, 1 and 2.
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 pos    [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv     [[attribute(2)]];
};
struct VertexOut {
    float4 pos [[position]];
    float2 uv  [[user(uv)]];
};

vertex VertexOut textured_vertex(VertexIn in [[stage_in]]) {
    VertexOut o;
    o.pos = float4(in.pos, 0.0, 1.0);
    o.uv = in.uv;
    return o;
}
