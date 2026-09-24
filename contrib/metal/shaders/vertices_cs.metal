// A triangle's three corners, scaled by a push constant, for pulled_vs.metal.
// Run with one thread a group.
#include <metal_stdlib>
using namespace metal;

kernel void vertices(device float2* positions [[buffer(0)]],
                     constant float& scale [[buffer(8)]],
                     uint i [[thread_position_in_grid]]) {
    const float2 corners[3] = { float2(0.0, 1.0), float2(-1.0, -1.0), float2(1.0, -1.0) };
    positions[i] = corners[i] * scale;
}
