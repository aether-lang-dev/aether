#ifndef AETHER_HOST_QUICKJS_H
#define AETHER_HOST_QUICKJS_H

#include <stdint.h>

int quickjs_run_sandboxed(void* perms, const char* code);
int quickjs_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token);
int quickjs_run(const char* code);
int quickjs_init(void);
void quickjs_finalize(void);

#endif
