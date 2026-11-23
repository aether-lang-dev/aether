#ifndef HBPROBE_H
#define HBPROBE_H
#include "aether_string_abi.h"
static inline int hb_probe_magic(void) { return AETHER_STRING_MAGIC == 0xAE57C0DE; }
#endif
