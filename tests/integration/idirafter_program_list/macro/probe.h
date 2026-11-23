#ifndef PROBE_H
#define PROBE_H
#define PROBE_ABI_HEADER "aether_string_abi.h"
#include PROBE_ABI_HEADER
static inline int probe_magic(void) { return AETHER_STRING_MAGIC == 0xAE57C0DE; }
#endif
