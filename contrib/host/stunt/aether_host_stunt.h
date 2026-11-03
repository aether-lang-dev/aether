#ifndef AETHER_HOST_STUNT_H
#define AETHER_HOST_STUNT_H

#include <stdint.h>

/* Tier 1, the surface every contrib host shares. */
int  stunt_init(void);
void stunt_finalize(void);
int  stunt_run(const char* code);
int  stunt_run_sandboxed(void* perms, const char* code);
int  stunt_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token);

/* Tier 2, the #904 bidirectional surface. */
void*       stunt_vm_new(void);
void        stunt_vm_free(void* vm);
int         stunt_vm_register(void* vm, const char* name, void* host_fn);
void        stunt_vm_set_map(void* vm, uint64_t map_token);
int         stunt_vm_eval(void* vm, const char* code);
int         stunt_vm_eval_sandboxed(void* vm, void* perms, const char* code);
const char* stunt_vm_result(void* vm);
void        stunt_vm_set_instruction_limit(void* vm, int64_t budget);
void        stunt_vm_cancel(void* vm);

/* Inside a registered host function. */
int         stunt_arg_count(void* vm);
int         stunt_arg_type(void* vm, int i);
const char* stunt_arg_str(void* vm, int i);
int64_t     stunt_arg_int(void* vm, int i);
void        stunt_push_str(void* vm, const char* v);
void        stunt_push_int(void* vm, int64_t v);
void        stunt_push_nil(void* vm);
int         stunt_raise_error(void* vm, const char* msg);

/* The shared map, for a host that has no other way to make one. */
void*       stunt_map_new(void);
uint64_t    stunt_map_token(void* map);
void        stunt_map_put(void* map, const char* key, const char* value);
const char* stunt_map_get(void* map, const char* key);
void        stunt_map_free(void* map);
void        stunt_map_freeze(void* map);
int         stunt_map_guest_put(uint64_t token, const char* key, const char* value);
const char* stunt_map_guest_get(uint64_t token, const char* key);
int         stunt_map_guest_has(uint64_t token, const char* key);

/* The catalogue. */
int         stunt_catalogue_size(void);
const char* stunt_catalogue_name(int i);
int         stunt_catalogue_host_checked(int i);

#endif
