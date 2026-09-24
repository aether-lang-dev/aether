// Vertices pulled from a storage buffer by index: no vertex input at all.
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 pos [[position]];
    float3 col [[user(col)]];
};

vertex VertexOut pulled_vertex(uint id [[vertex_id]], device const float2* positions [[buffer(0)]]) {
    VertexOut o;
    o.pos = float4(positions[id], 0.0, 1.0);
    o.col = float3(1.0, 0.5, 0.0);
    return o;
}
