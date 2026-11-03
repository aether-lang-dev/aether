// aether_host_stunt.c: the bridge to stunt, the test double (README.md).
//
// Loads libstunt the way contrib/host/lua loads liblua: dlopen at the first
// call, every engine function resolved with dlsym, nothing to link. What it
// does with it is what every in-process bridge must do with an interpreter:
//
//   - run_sandboxed pushes the grants as a level of the calling thread
//     (aether_sandbox_push) and pops it after, so a bridge called inside an
//     enforce block is held to both (#2955);
//   - on entry it scrubs the engine's cached copy of the environment, the
//     step Perl's and Ruby's bridges take for %ENV and ENV;
//   - it gives the engine host functions: read_file and getenv, which check
//     the grants themselves (the native-bindings model of the Duktape and
//     QuickJS bridges), and aether_map_get / aether_map_put on the shared
//     map, whose inputs it freezes first;
//   - the #904 tier: a persistent VM, host callbacks with typed arguments and
//     results, and an instruction limit.
//
// LD_PRELOAD-model checks (open, connect, execve, getenv through libc) are
// held by libaether_sandbox.so when the host runs under it; this bridge
// pushes the levels that library consults.

#include "aether_host_stunt.h"
#include "stunt.h"
#include "../aep_dl.h"
#include "../../../runtime/aether_sandbox.h"
#include "../../../runtime/aether_shared_map.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    void* lib;
    st_vm* (*new_vm)(void);
    void (*free_vm)(st_vm*);
    int (*reg)(st_vm*, const char*, st_host_fn);
    int (*eval)(st_vm*, const char*);
    const char* (*result)(st_vm*);
    void (*set_limit)(st_vm*, int64_t);
    void (*cancel)(st_vm*);
    void (*env_scrub)(st_vm*, int (*)(const char*));
    int (*cat_size)(void);
    const char* (*cat_name)(int);
    int (*cat_host_checked)(int);
    int (*arg_count)(void*);
    int (*arg_type)(void*, int);
    const char* (*arg_str)(void*, int);
    int64_t (*arg_int)(void*, int);
    void (*push_str)(void*, const char*);
    void (*push_int)(void*, int64_t);
    void (*push_nil)(void*);
    int (*raise_error)(void*, const char*);
} S;

#if defined(_WIN32)
#define STUNT_DEFAULT_LIB "stunt.dll"
#elif defined(__APPLE__)
#define STUNT_DEFAULT_LIB "libstunt.dylib"
#else
#define STUNT_DEFAULT_LIB "libstunt.so"
#endif

#define RESOLVE(field, sym) \
    do { \
        *(void**)(&S.field) = dlsym(S.lib, sym); \
        if (!S.field) { \
            fprintf(stderr, "[stunt] %s: missing symbol %s\n", name, sym); \
            dlclose(S.lib); \
            S.lib = NULL; \
            return -1; \
        } \
    } while (0)

// AETHER_STUNT_LIB names the library (a path, or a name for the loader to
// find); without it, libstunt.so / libstunt.dylib / stunt.dll on the
// loader's own search path.
static int load_libstunt(void) {
    if (S.lib) return 0;
    const char* name = getenv("AETHER_STUNT_LIB");
    if (!name || !*name) name = STUNT_DEFAULT_LIB;
    S.lib = dlopen(name, RTLD_NOW);
    if (!S.lib) {
        const char* why = dlerror();
        fprintf(stderr, "[stunt] cannot load %s (set AETHER_STUNT_LIB): %s\n", name, why ? why : "(none)");
        return -1;
    }
    RESOLVE(new_vm, "st_new");
    RESOLVE(free_vm, "st_free");
    RESOLVE(reg, "st_register");
    RESOLVE(eval, "st_eval");
    RESOLVE(result, "st_result");
    RESOLVE(set_limit, "st_set_instruction_limit");
    RESOLVE(cancel, "st_cancel");
    RESOLVE(env_scrub, "st_env_scrub");
    RESOLVE(cat_size, "st_catalogue_size");
    RESOLVE(cat_name, "st_catalogue_name");
    RESOLVE(cat_host_checked, "st_catalogue_host_checked");
    RESOLVE(arg_count, "st_arg_count");
    RESOLVE(arg_type, "st_arg_type");
    RESOLVE(arg_str, "st_arg_str");
    RESOLVE(arg_int, "st_arg_int");
    RESOLVE(push_str, "st_push_str");
    RESOLVE(push_int, "st_push_int");
    RESOLVE(push_nil, "st_push_nil");
    RESOLVE(raise_error, "st_raise_error");
    return 0;
}

// ---- the callback side ------------------------------------------------------

int stunt_arg_count(void* vm) { return S.lib ? S.arg_count(vm) : 0; }
int stunt_arg_type(void* vm, int i) { return S.lib ? S.arg_type(vm, i) : 0; }
const char* stunt_arg_str(void* vm, int i) { return S.lib ? S.arg_str(vm, i) : ""; }
int64_t stunt_arg_int(void* vm, int i) { return S.lib ? S.arg_int(vm, i) : 0; }
void stunt_push_str(void* vm, const char* v) { if (S.lib) S.push_str(vm, v); }
void stunt_push_int(void* vm, int64_t v) { if (S.lib) S.push_int(vm, v); }
void stunt_push_nil(void* vm) { if (S.lib) S.push_nil(vm); }
int stunt_raise_error(void* vm, const char* msg) { return S.lib ? S.raise_error(vm, msg) : -1; }

// ---- host functions every VM gets --------------------------------------------

#ifdef AETHER_HAS_SANDBOX
#define ALLOWED(cat, res) aether_sandbox_check((cat), (res))
#else
#define ALLOWED(cat, res) ((void)(cat), (void)(res), 1)
#endif

// read_file(path): the file's first 4 KiB, if the grants allow reading it.
static int host_read_file(void* vm) {
    const char* path = S.arg_str(vm, 1);
    char msg[512];
    if (!ALLOWED("fs_read", path)) {
        snprintf(msg, sizeof msg, "refused: fs_read %s", path);
        return S.raise_error(vm, msg);
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(msg, sizeof msg, "%s: %s", path, strerror(errno));
        return S.raise_error(vm, msg);
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    S.push_str(vm, buf);
    return 1;
}

// getenv(name): the variable, if the grants allow reading it.
static int host_getenv(void* vm) {
    const char* name = S.arg_str(vm, 1);
    if (!ALLOWED("env", name)) {
        char msg[300];
        snprintf(msg, sizeof msg, "refused: env %s", name);
        return S.raise_error(vm, msg);
    }
    const char* v = getenv(name);
    if (v) S.push_str(vm, v);
    else S.push_nil(vm);
    return 1;
}

// The shared map: one token per VM, set by stunt_vm_set_map.
#define STUNT_MAX_VMS 16
static struct { void* vm; uint64_t token; } g_maps[STUNT_MAX_VMS];

static uint64_t map_token_of(void* vm) {
    for (int i = 0; i < STUNT_MAX_VMS; i++) {
        if (g_maps[i].vm == vm) return g_maps[i].token;
    }
    return 0;
}

static void set_map_token(void* vm, uint64_t token) {
    int free_slot = -1;
    for (int i = 0; i < STUNT_MAX_VMS; i++) {
        if (g_maps[i].vm == vm) { g_maps[i].token = token; return; }
        if (!g_maps[i].vm && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        g_maps[free_slot].vm = vm;
        g_maps[free_slot].token = token;
    }
}

static void forget_vm(void* vm) {
    for (int i = 0; i < STUNT_MAX_VMS; i++) {
        if (g_maps[i].vm == vm) { g_maps[i].vm = NULL; g_maps[i].token = 0; }
    }
}

static int host_map_get(void* vm) {
    const char* v = aether_shared_map_get_by_token(map_token_of(vm), S.arg_str(vm, 1));
    if (v) S.push_str(vm, v);
    else S.push_nil(vm);
    return 1;
}

static int host_map_put(void* vm) {
    S.push_int(vm, aether_shared_map_put_by_token(map_token_of(vm), S.arg_str(vm, 1), S.arg_str(vm, 2)));
    return 1;
}

// ---- tier 2 ------------------------------------------------------------------

void* stunt_vm_new(void) {
    if (load_libstunt() != 0) return NULL;
    st_vm* vm = S.new_vm();
    if (!vm) return NULL;
    S.reg(vm, "read_file", host_read_file);
    S.reg(vm, "getenv", host_getenv);
    return vm;
}

void stunt_vm_free(void* vm) {
    if (!vm || !S.lib) return;
    forget_vm(vm);
    S.free_vm((st_vm*)vm);
}

int stunt_vm_register(void* vm, const char* name, void* host_fn) {
    if (!vm || !name || !host_fn || !S.lib) return -1;
    return S.reg((st_vm*)vm, name, (st_host_fn)host_fn);
}

// Gives the VM the shared map behind `token`, its inputs frozen first: the
// guest reads them and adds keys of its own, and cannot overwrite one.
void stunt_vm_set_map(void* vm, uint64_t token) {
    if (!vm || !S.lib) return;
    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(token);
    set_map_token(vm, token);
    S.reg((st_vm*)vm, "aether_map_get", host_map_get);
    S.reg((st_vm*)vm, "aether_map_put", host_map_put);
}

int stunt_vm_eval(void* vm, const char* code) {
    if (!vm || !code || !S.lib) return -1;
    return S.eval((st_vm*)vm, code);
}

#ifdef AETHER_HAS_SANDBOX
static int env_granted(const char* name) { return aether_sandbox_check("env", name); }
#endif

// Runs `code` with `perms` pushed as a level of this thread's sandbox: under
// it, the host functions above check against every level in force, and
// libaether_sandbox.so, when the host runs under it, holds the engine's
// libc calls to them too. The level is popped however the run ends.
int stunt_vm_eval_sandboxed(void* vm, void* perms, const char* code) {
    if (!vm || !code || !S.lib) return -1;
#ifdef AETHER_HAS_SANDBOX
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;
    aether_sandbox_push(perms);
    S.env_scrub((st_vm*)vm, env_granted);
    int rc = S.eval((st_vm*)vm, code);
    aether_sandbox_pop();
    return rc;
#else
    (void)perms;
    fprintf(stderr, "[stunt] built without AETHER_HAS_SANDBOX: refusing a sandboxed run\n");
    return -1;
#endif
}

const char* stunt_vm_result(void* vm) { return (vm && S.lib) ? S.result((st_vm*)vm) : ""; }
void stunt_vm_set_instruction_limit(void* vm, int64_t budget) { if (vm && S.lib) S.set_limit((st_vm*)vm, budget); }
void stunt_vm_cancel(void* vm) { if (vm && S.lib) S.cancel((st_vm*)vm); }

// ---- tier 1: one VM, the line printed --------------------------------------

static st_vm* g_vm = NULL;

int stunt_init(void) {
    if (g_vm) return 0;
    g_vm = (st_vm*)stunt_vm_new();
    return g_vm ? 0 : -1;
}

void stunt_finalize(void) {
    if (g_vm) {
        stunt_vm_free(g_vm);
        g_vm = NULL;
    }
}

static int print_result(int rc) {
    if (g_vm) {
        printf("%s\n", S.result(g_vm));
        fflush(stdout);
    }
    return rc;
}

int stunt_run(const char* code) {
    if (!code || stunt_init() != 0) return -1;
    return print_result(S.eval(g_vm, code));
}

int stunt_run_sandboxed(void* perms, const char* code) {
    if (!code || stunt_init() != 0) return -1;
    return print_result(stunt_vm_eval_sandboxed(g_vm, perms, code));
}

int stunt_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code || stunt_init() != 0) return -1;
    stunt_vm_set_map(g_vm, map_token);
    int rc = stunt_vm_eval_sandboxed(g_vm, perms, code);
    set_map_token(g_vm, 0);
    return print_result(rc);
}

// ---- the shared map, for hosts ----------------------------------------------

typedef struct { AetherSharedMap* map; uint64_t token; } StuntMap;

void* stunt_map_new(void) {
    StuntMap* m = (StuntMap*)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->map = aether_shared_map_new(&m->token);
    if (!m->map) { free(m); return NULL; }
    return m;
}

uint64_t stunt_map_token(void* map) { return map ? ((StuntMap*)map)->token : 0; }

void stunt_map_put(void* map, const char* key, const char* value) {
    if (map) aether_shared_map_put(((StuntMap*)map)->map, key, value);
}

const char* stunt_map_get(void* map, const char* key) {
    return map ? aether_shared_map_get(((StuntMap*)map)->map, key) : NULL;
}

void stunt_map_free(void* map) {
    if (!map) return;
    StuntMap* m = (StuntMap*)map;
    aether_shared_map_revoke_token(m->token);
    aether_shared_map_free(m->map);
    free(m);
}

// Freezes the keys put so far: a guest can read them and not overwrite them.
void stunt_map_freeze(void* map) {
    if (map) aether_shared_map_freeze_inputs(((StuntMap*)map)->map);
}

// What a guest in a child process asks for over the socket, applied by its
// host with the guest's rules: inputs frozen, outputs added.
int stunt_map_guest_put(uint64_t token, const char* key, const char* value) {
    return aether_shared_map_put_by_token(token, key, value);
}

const char* stunt_map_guest_get(uint64_t token, const char* key) {
    return aether_shared_map_get_by_token(token, key);
}

int stunt_map_guest_has(uint64_t token, const char* key) {
    return aether_shared_map_get_by_token(token, key) != NULL;
}

// ---- the catalogue -----------------------------------------------------------

int stunt_catalogue_size(void) { return load_libstunt() == 0 ? S.cat_size() : 0; }
const char* stunt_catalogue_name(int i) { return load_libstunt() == 0 ? S.cat_name(i) : NULL; }
int stunt_catalogue_host_checked(int i) { return load_libstunt() == 0 ? S.cat_host_checked(i) : 0; }
