// A float4x4 pushed per draw ([[buffer(8)]]): the same sixteen floats, in the
// same order (column-major), as contrib.vulkan's transform.vert takes as its
// push block.
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

vertex VertexOut transform_vertex(VertexIn in [[stage_in]], constant float4x4& mvp [[buffer(8)]]) {
    VertexOut o;
    o.pos = mvp * float4(in.pos, 0.0, 1.0);
    o.col = in.col;
    return o;
}
