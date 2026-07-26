#include "aether_msgpack.h"
#include <string.h>

uint32_t aether_msgpack_float_to_bits(double val) {
    float f = (float)val;
    uint32_t bits;
    memcpy(&bits, &f, sizeof(float));
    return bits;
}

double aether_msgpack_bits_to_float(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof(float));
    return (double)f;
}

uint64_t aether_msgpack_double_to_bits(double val) {
    uint64_t bits;
    memcpy(&bits, &val, sizeof(double));
    return bits;
}

double aether_msgpack_bits_to_double(uint64_t bits) {
    double val;
    memcpy(&val, &bits, sizeof(double));
    return val;
}
