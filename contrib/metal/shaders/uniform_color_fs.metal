// The colour in the draw's window of a dynamic uniform buffer (#2198): the
// same binding ([[buffer(0)]]) reads a different block for each draw.
#include <metal_stdlib>
using namespace metal;

struct Colour {
    float4 c;
};

fragment float4 uniform_color_fragment(float4 pos [[position]],
                                       constant Colour& colour [[buffer(0)]]) {
    return colour.c;
}
