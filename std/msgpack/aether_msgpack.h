#ifndef AETHER_MSGPACK_H
#define AETHER_MSGPACK_H

#include <stdint.h>

uint32_t aether_msgpack_float_to_bits(double val);
double aether_msgpack_bits_to_float(uint32_t bits);
uint64_t aether_msgpack_double_to_bits(double val);
double aether_msgpack_bits_to_double(uint64_t bits);

#endif
