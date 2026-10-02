// The colour in the draw's window of a dynamic uniform buffer (#2198): the
// same binding (b0) reads a different block for each draw.
cbuffer Colour : register(b0) {
    float4 c;
};

float4 main(float4 pos : SV_Position) : SV_Target {
    return c;
}
