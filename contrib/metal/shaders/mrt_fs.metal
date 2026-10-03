// Three outputs, one an attachment (#2386): the interpolated colour, a fixed
// colour, and an HDR one a float attachment keeps whole.
#include <metal_stdlib>
using namespace metal;

struct FragmentIn {
    float4 pos [[position]];
    float3 col [[user(col)]];
};
struct FragmentOut {
    float4 c0 [[color(0)]];
    float4 c1 [[color(1)]];
    float4 c2 [[color(2)]];
};

fragment FragmentOut mrt_fragment(FragmentIn in [[stage_in]]) {
    FragmentOut o;
    o.c0 = float4(in.col, 1.0);
    o.c1 = float4(0.25, 0.5, 0.75, 1.0);
    o.c2 = float4(2.0, -1.0, 0.5, 1.0);
    return o;
}
