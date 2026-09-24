// The interpolated colour, opaque. Vertex outputs reach it by their
// [[user(...)]] names, so any vertex shader here pairs with it.
#include <metal_stdlib>
using namespace metal;

struct FragmentIn {
    float4 pos [[position]];
    float3 col [[user(col)]];
};

fragment float4 triangle_fragment(FragmentIn in [[stage_in]]) {
    return float4(in.col, 1.0);
}
