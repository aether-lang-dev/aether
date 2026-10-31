#ifndef AETHER_HOST_DUKTAPE_H
#define AETHER_HOST_DUKTAPE_H

#include <stdint.h>

// Tier 1: fire-and-forget
int duktape_run_sandboxed(void* perms, const char* code);
int duktape_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token);
int duktape_run(const char* code);
int duktape_init(void);
void duktape_finalize(void);

// Tier 2: bidirectional API (#904)
void* duktape_vm_new(void);
void duktape_vm_free(void* c);
int duktape_vm_register(void* c, const char* name, void* host_fn);
void duktape_vm_set_global_str(void* c, const char* name, const char* value);
void duktape_vm_set_global_int(void* c, const char* name, int64_t value);
void duktape_vm_set_global_strlist(void* c, const char* name, void* items);
int duktape_vm_eval(void* c, const char* code);
int duktape_vm_result_type(void* c);
int64_t duktape_vm_result_int(void* c);
const char* duktape_vm_result_str(void* c);
int duktape_vm_result_bool(void* c);
void duktape_vm_pop_result(void* c);
const char* duktape_vm_set_instruction_limit(void* c, int64_t budget, int every);

// Callback-side stack API
int duktape_arg_count(void* c);
int duktape_arg_type(void* c, int i);
const char* duktape_arg_str(void* c, int i);
int64_t duktape_arg_int(void* c, int i);
int duktape_arg_bool(void* c, int i);
void duktape_push_int(void* c, int64_t v);
void duktape_push_str(void* c, const char* v);
void duktape_push_bool(void* c, int v);
void duktape_push_nil(void* c);
void duktape_push_double(void* c, double v);
int duktape_raise_error(void* c, const char* msg);

#endif
