#ifndef PROBE_H
#define PROBE_H
#include "aether_string_abi.h"
static inline int probe_magic(void) { return AETHER_STRING_MAGIC == 0xAE57C0DE; }
#endif
