// Writes voxel (x, y, z) of a 2x2x2 RGBA8 storage texture as (x, y, z, 1)
// (#2388): the corners of the colour cube, exact in eight bits.
RWTexture3D<unorm float4> vol : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    vol[id] = float4(float3(id), 1.0);
}
