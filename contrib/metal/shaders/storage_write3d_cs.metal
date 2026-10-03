// Writes voxel (x, y, z) of a 2x2x2 RGBA8 storage texture as (x, y, z, 1)
// (#2388): the corners of the colour cube, exact in eight bits. Run with one
// thread a group.
#include <metal_stdlib>
using namespace metal;

kernel void storage_write3d(texture3d<float, access::write> vol [[texture(0)]],
                            uint3 p [[thread_position_in_grid]]) {
    vol.write(float4(float3(p), 1.0), p);
}
