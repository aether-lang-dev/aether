#ifndef PROBE_H
#define PROBE_H
#include "Aether_String_ABI.h"
static inline int probe_magic(void) { return AETHER_STRING_MAGIC == 0xAE57C0DE; }
#endif
