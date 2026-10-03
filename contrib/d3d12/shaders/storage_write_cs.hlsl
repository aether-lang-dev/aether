// Writes texel (x, y) of a float storage texture as (x/4, y/4, 0.5, 1)
// (#2388): values a 4x4 texture holds exactly, read back by a draw.
RWTexture2D<float4> img : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    img[id.xy] = float4(id.x * 0.25, id.y * 0.25, 0.5, 1.0);
}
