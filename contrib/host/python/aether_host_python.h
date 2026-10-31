#ifndef AETHER_HOST_PYTHON_H
#define AETHER_HOST_PYTHON_H

#include <stdint.h>

// Tier 1: fire-and-forget
int python_run_sandboxed(void* perms, const char* code);
int python_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token);
int python_run(const char* code);
int python_init(void);
void python_finalize(void);

// Tier 2: bidirectional API (#904)
void* python_vm_new(void);
void python_vm_free(void* vm);
int python_vm_register(void* vm, const char* name, void* host_fn);
void python_vm_set_global_str(void* vm, const char* name, const char* value);
void python_vm_set_global_int(void* vm, const char* name, int64_t value);
void python_vm_set_global_strlist(void* vm, const char* name, void* items);
int python_vm_eval(void* vm, const char* code);
int python_vm_result_type(void* vm);
int64_t python_vm_result_int(void* vm);
const char* python_vm_result_str(void* vm);
int python_vm_result_bool(void* vm);
double python_vm_result_double(void* vm);
void* python_vm_result_bytes(void* vm);   // a new std.bytes handle, or NULL
void python_vm_pop_result(void* vm);
const char* python_vm_set_instruction_limit(void* vm, int64_t budget, int every);

// Callback-side stack API (used INSIDE a registered host fn)
int python_arg_count(void* vm);
int python_arg_type(void* vm, int i);
const char* python_arg_str(void* vm, int i);
int64_t python_arg_int(void* vm, int i);
int python_arg_bool(void* vm, int i);
double python_arg_double(void* vm, int i);
void* python_arg_bytes(void* vm, int i);  // a new std.bytes handle, or NULL

// Pushers
void python_push_int(void* vm, int64_t v);
void python_push_str(void* vm, const char* v);
void python_push_bool(void* vm, int v);
void python_push_nil(void* vm);
void python_push_double(void* vm, double v);
void python_push_bytes(void* vm, void* bytes);   // a std.bytes handle, copied
int python_raise_error(void* vm, const char* msg);

#endif
