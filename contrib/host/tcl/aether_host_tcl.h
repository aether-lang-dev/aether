#ifndef AETHER_HOST_TCL_H
#define AETHER_HOST_TCL_H

#include <stdint.h>

int tcl_run_sandboxed(void* perms, const char* code);
int tcl_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token);
int tcl_run(const char* code);
int tcl_init(void);
void tcl_finalize(void);

// #904 bidirectional tier: a persistent interpreter per VM, host callbacks
// as Tcl commands, typed values both ways.
void* tcl_vm_new(void);
void tcl_vm_free(void* vm);
int tcl_vm_register(void* vm, const char* name, void* host_fn);
void tcl_vm_set_global_str(void* vm, const char* name, const char* value);
void tcl_vm_set_global_int(void* vm, const char* name, int64_t value);
void tcl_vm_set_global_strlist(void* vm, const char* name, void* items);
int tcl_vm_eval(void* vm, const char* code);
int tcl_vm_result_type(void* vm);
int64_t tcl_vm_result_int(void* vm);
double tcl_vm_result_double(void* vm);
const char* tcl_vm_result_str(void* vm);
int tcl_vm_result_bool(void* vm);
void tcl_vm_pop_result(void* vm);
const char* tcl_vm_set_instruction_limit(void* vm, int64_t budget, int every);
const char* tcl_vm_set_time_limit(void* vm, int64_t ms, int every);
int tcl_arg_count(void* vm);
int tcl_arg_type(void* vm, int i);
int64_t tcl_arg_int(void* vm, int i);
double tcl_arg_double(void* vm, int i);
int tcl_arg_bool(void* vm, int i);
const char* tcl_arg_str(void* vm, int i);
void tcl_push_int(void* vm, int64_t v);
void tcl_push_double(void* vm, double v);
void tcl_push_bool(void* vm, int v);
void tcl_push_str(void* vm, const char* v);
void tcl_push_nil(void* vm);
int tcl_raise_error(void* vm, const char* msg);

#endif
