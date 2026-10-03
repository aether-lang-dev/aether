// Writes texel (x, y) of a float storage texture as (x/4, y/4, 0.5, 1)
// (#2388): values a 4x4 texture holds exactly, read back by a draw. Run with
// one thread a group.
#include <metal_stdlib>
using namespace metal;

kernel void storage_write(texture2d<float, access::write> img [[texture(0)]],
                          uint2 p [[thread_position_in_grid]]) {
    img.write(float4(float(p.x) * 0.25, float(p.y) * 0.25, 0.5, 1.0), p);
}
