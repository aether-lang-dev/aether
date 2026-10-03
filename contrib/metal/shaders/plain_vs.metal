// Position only: what the fragment shader writes comes from elsewhere.
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 pos [[attribute(0)]];
};

vertex float4 plain_vertex(VertexIn in [[stage_in]]) {
    return float4(in.pos, 0.0, 1.0);
}
