// aether_host_lua.c — Embedded Lua Language Host Module
//
// Embeds Lua in the Aether process. When run_sandboxed is called,
// installs the Aether sandbox checker so Lua's libc calls are
// intercepted and checked against the grant list.
//
// LOADING MODEL: dlopen, not -llua. Mirrors contrib/host/python and
// contrib/host/ruby — every Lua C-API symbol resolved via dlsym at
// first call. The bridge .a has NO unresolved Lua symbols at link
// time, so end-user binaries have no DT_NEEDED liblua and are
// ABI-portable across deploy-host Lua minor versions (5.3 / 5.4).
//
// Lua's headers use a lot of macros that are just one-line wrappers
// around real exported functions:
//
//   luaL_dostring(L,s)      → luaL_loadstring(L,s) || lua_pcall(L,0,LUA_MULTRET,0)
//   lua_pcall(L,n,r,f)      → lua_pcallk(L,n,r,f,0,NULL)
//   lua_tostring(L,i)       → lua_tolstring(L,i,NULL)
//   lua_pop(L,n)            → lua_settop(L,-(n)-1)
//   lua_pushcfunction(L,f)  → lua_pushcclosure(L,f,0)
//   luaL_checkstring(L,n)   → luaL_checklstring(L,n,NULL)
//
// We dlsym the wrapped functions and re-implement the macros as
// inline static functions over our g_lua table. The wrapper-macros
// are NOT dlsym'd directly — they don't exist as real symbols.
//
// Symbol versioning: liblua uses `lua_close@@LUA_5.4`-style
// versioning. dlsym strips version tags and returns whatever match
// it finds, so `dlsym(h, "lua_close")` works against 5.3 or 5.4
// libluas indifferently.

#include "aether_host_lua.h"
#include "../../../runtime/aether_sandbox.h"
#include "../../../runtime/aether_shared_map.h"

#ifdef AETHER_HAS_LUA
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

/* Lua 5.4+ defines luaL_openlibs as a function-like macro:
 *   #define luaL_openlibs(L) luaL_openselectedlibs(L, ~0, 0)
 * Left in place, it rewrites our struct-field call
 * `g_lua.luaL_openlibs(L)` (and the RESOLVE(luaL_openlibs, ...)
 * registration) into a reference to a non-existent
 * `luaL_openselectedlibs` member — the macOS/Homebrew Lua 5.4 build
 * break. We dlsym the real exported `luaL_openlibs` symbol, which
 * every shipping liblua provides, so undo the macro and call it as a
 * plain function pointer. (Lua 5.3 has no such macro; the #undef is a
 * harmless no-op there — matching the version-indifference this bridge
 * is built for.) */
#ifdef luaL_openlibs
#undef luaL_openlibs
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- liblua dlopen table ----------------------------------------------------

static void* liblua_handle = NULL;

static struct {
    // Lifecycle.
    lua_State* (*luaL_newstate)(void);
    void       (*luaL_openlibs)(lua_State*);
    void       (*lua_close)(lua_State*);
    // Code load + execute (the underlying fns the macros wrap).
    int        (*luaL_loadstring)(lua_State*, const char*);
    int        (*lua_pcallk)(lua_State*, int nargs, int nresults, int errfunc,
                             lua_KContext ctx, lua_KFunction k);
    // Stack manipulation (function form of lua_pop's underlying call).
    void       (*lua_settop)(lua_State*, int idx);
    // Value pushers.
    void       (*lua_pushstring)(lua_State*, const char*);
    void       (*lua_pushnil)(lua_State*);
    void       (*lua_pushcclosure)(lua_State*, lua_CFunction, int n);
    // Globals / value extraction.
    void       (*lua_setglobal)(lua_State*, const char*);
    const char* (*lua_tolstring)(lua_State*, int idx, size_t* len);
    // luaL_checkstring wrappee.
    const char* (*luaL_checklstring)(lua_State*, int arg, size_t* l);

    // --- #904 bidirectional surface ---------------------------------------
    // Typed value pushers / readers for the host-callback + typed-eval API.
    void       (*lua_pushinteger)(lua_State*, lua_Integer);
    void       (*lua_pushboolean)(lua_State*, int);
    void       (*lua_pushlstring)(lua_State*, const char*, size_t);
    lua_Integer (*lua_tointegerx)(lua_State*, int idx, int* isnum);
    int        (*lua_toboolean)(lua_State*, int idx);
    int        (*lua_type)(lua_State*, int idx);
    int        (*lua_gettop)(lua_State*);
    void       (*lua_pushvalue)(lua_State*, int idx);
    // Globals (read side) + tables.
    int        (*lua_getglobal)(lua_State*, const char*);
    void       (*lua_createtable)(lua_State*, int narr, int nrec);
    void       (*lua_setfield)(lua_State*, int idx, const char*);
    int        (*lua_getfield)(lua_State*, int idx, const char*);
    void       (*lua_seti)(lua_State*, int idx, lua_Integer n);
    // Error + light-userdata (carries the per-callback host fn pointer
    // through a C closure upvalue).
    int        (*lua_error)(lua_State*);
    void       (*lua_pushlightuserdata)(lua_State*, void*);
    void*      (*lua_touserdata)(lua_State*, int idx);
    // Execution guard: instruction-count hook.
    void       (*lua_sethook)(lua_State*, lua_Hook, int mask, int count);
} g_lua;

static int resolve_lua_symbols(void* h) {
#define RESOLVE(field, sym) do {                                       \
        *(void**)(&g_lua.field) = dlsym(h, sym);                       \
        if (!g_lua.field) {                                            \
            fprintf(stderr,                                            \
                "aether host_lua: liblua missing symbol %s\n", sym);   \
            return -1;                                                 \
        }                                                              \
    } while (0)

    RESOLVE(luaL_newstate,     "luaL_newstate");
    RESOLVE(luaL_openlibs,     "luaL_openlibs");
    RESOLVE(lua_close,         "lua_close");
    RESOLVE(luaL_loadstring,   "luaL_loadstring");
    RESOLVE(lua_pcallk,        "lua_pcallk");
    RESOLVE(lua_settop,        "lua_settop");
    RESOLVE(lua_pushstring,    "lua_pushstring");
    RESOLVE(lua_pushnil,       "lua_pushnil");
    RESOLVE(lua_pushcclosure,  "lua_pushcclosure");
    RESOLVE(lua_setglobal,     "lua_setglobal");
    RESOLVE(lua_tolstring,     "lua_tolstring");
    RESOLVE(luaL_checklstring, "luaL_checklstring");
    // #904 bidirectional surface.
    RESOLVE(lua_pushinteger,        "lua_pushinteger");
    RESOLVE(lua_pushboolean,        "lua_pushboolean");
    RESOLVE(lua_pushlstring,        "lua_pushlstring");
    RESOLVE(lua_tointegerx,         "lua_tointegerx");
    RESOLVE(lua_toboolean,          "lua_toboolean");
    RESOLVE(lua_type,               "lua_type");
    RESOLVE(lua_gettop,             "lua_gettop");
    RESOLVE(lua_pushvalue,          "lua_pushvalue");
    RESOLVE(lua_getglobal,          "lua_getglobal");
    RESOLVE(lua_createtable,        "lua_createtable");
    RESOLVE(lua_setfield,           "lua_setfield");
    RESOLVE(lua_getfield,           "lua_getfield");
    RESOLVE(lua_seti,               "lua_seti");
    RESOLVE(lua_error,              "lua_error");
    RESOLVE(lua_pushlightuserdata,  "lua_pushlightuserdata");
    RESOLVE(lua_touserdata,         "lua_touserdata");
    RESOLVE(lua_sethook,            "lua_sethook");
    return 0;
#undef RESOLVE
}

static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

// Load liblua on first use. Strict two-step contract:
//   1. ${AETHER_LUA_SONAME} env var (orchestrator-supplied exact,
//      e.g. "liblua5.4.so.0"). Lua doesn't have a standard runtime
//      sysconfig-style probe — the orchestrator can read
//      `lua -e 'print(_VERSION)'` to learn the major.minor, then
//      check the distro's actual sonames (or just rely on the
//      unversioned symlink below).
//   2. liblua.so (Debian/Fedora unversioned symlink, when present).
// No hardcoded version-list fallback. The orchestrator owns the
// probe; the bridge stays distro-agnostic.
static int load_liblua(void) {
    if (liblua_handle) return 0;

    const char* env_soname = getenv("AETHER_LUA_SONAME");
    void* h = try_dlopen(env_soname);
    if (!h) h = try_dlopen("liblua.so");
    if (!h) {
        fprintf(stderr,
            "aether host_lua: cannot dlopen liblua "
            "(tried $AETHER_LUA_SONAME=%s, liblua.so).\n"
            "  Install a lua runtime on the host, or set "
            "AETHER_LUA_SONAME to the exact soname "
            "(e.g. liblua5.4.so.0).\n  dlerror: %s\n",
            env_soname ? env_soname : "(unset)",
            dlerror() ? dlerror() : "(none)");
        return -1;
    }

    if (resolve_lua_symbols(h) != 0) {
        dlclose(h);
        return -1;
    }
    liblua_handle = h;
    return 0;
}

// --- macro re-expressions (these are #defines in lua.h, not symbols) -------

#define LH_LUA_MULTRET (-1)  /* = LUA_MULTRET */

// luaL_dostring(L,s) → loadstring + pcall.
static int lh_dostring(lua_State* L, const char* s) {
    if (g_lua.luaL_loadstring(L, s) != 0) return 1;
    return g_lua.lua_pcallk(L, 0, LH_LUA_MULTRET, 0, 0, NULL);
}

// lua_pop(L,n) → lua_settop(L, -(n)-1).
static void lh_pop(lua_State* L, int n) {
    g_lua.lua_settop(L, -(n) - 1);
}

// lua_tostring(L,i) → lua_tolstring(L,i,NULL).
static const char* lh_tostring(lua_State* L, int idx) {
    return g_lua.lua_tolstring(L, idx, NULL);
}

// luaL_checkstring(L,n) → luaL_checklstring(L,n,NULL).
static const char* lh_checkstring(lua_State* L, int arg) {
    return g_lua.luaL_checklstring(L, arg, NULL);
}

// lua_pushcfunction(L,f) → lua_pushcclosure(L,f,0).
static void lh_pushcfunction(lua_State* L, lua_CFunction fn) {
    g_lua.lua_pushcclosure(L, fn, 0);
}

// --- bridge state (unchanged) ----------------------------------------------

static lua_State* L = NULL;

static void* lua_perms_stack[64];
static int   lua_perms_depth = 0;

extern int list_size(void*);
extern void* list_get_raw(void*, int);
/* A grant list std.sandbox built holds plain C strings; one a program
 * built can hold Aether strings (a helper that keeps its `string`
 * parameter stores its own copy). This reads the bytes of either. */
extern const char* aether_string_data(const void*);

// The shared grant matcher (runtime/aether_sandbox_match.h).
#include "aether_sandbox_match.h"

static int perms_allow(void* ctx, const char* category, const char* resource) {
    if (!ctx) return 1;
    int n = list_size(ctx);
    if (n == 0) return 0;
    for (int i = 0; i < n; i += 2) {
        const char* cat = aether_string_data(list_get_raw(ctx, i));
        const char* pat = aether_string_data(list_get_raw(ctx, i + 1));
        if (!cat || !pat) continue;
        if (cat[0] == '*' && pat[0] == '*') return 1;
        if (strcmp(cat, category) == 0 && aether_grant_match(category, pat, resource)) return 1;
    }
    return 0;
}

static int host_lua_checker(const char* category, const char* resource) {
    if (lua_perms_depth <= 0) return 1;
    for (int level = 0; level < lua_perms_depth; level++) {
        if (!perms_allow(lua_perms_stack[level], category, resource)) return 0;
    }
    return 1;
}

int lua_init(void) {
    if (L) return 0;
    if (load_liblua() != 0) return -1;
    L = g_lua.luaL_newstate();
    if (!L) return -1;
    g_lua.luaL_openlibs(L);
    return 0;
}

void lua_finalize(void) {
    if (L) {
        g_lua.lua_close(L);
        L = NULL;
    }
}

int lua_run(const char* code) {
    if (!code) return -1;
    if (lua_init() != 0) return -1;
    if (lh_dostring(L, code) != 0) {
        fprintf(stderr, "[lua] %s\n", lh_tostring(L, -1));
        lh_pop(L, 1);
        return -1;
    }
    return 0;
}

int lua_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (lua_init() != 0) return -1;
    if (lua_perms_depth >= 64) return -1;

    lua_perms_stack[lua_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = host_lua_checker;

    int result = 0;
    if (lh_dostring(L, code) != 0) {
        fprintf(stderr, "[lua] %s\n", lh_tostring(L, -1));
        lh_pop(L, 1);
        result = -1;
    }

    _aether_sandbox_checker = prev;
    lua_perms_depth--;

    return result;
}

// --- Shared map native bindings for Lua ------------------------------------

static uint64_t current_map_token = 0;

static int lua_aether_map_get(lua_State* state) {
    const char* key = lh_checkstring(state, 1);
    const char* val = aether_shared_map_get_by_token(current_map_token, key);
    if (val) {
        g_lua.lua_pushstring(state, val);
    } else {
        g_lua.lua_pushnil(state);
    }
    return 1;
}

static int lua_aether_map_put(lua_State* state) {
    const char* key = lh_checkstring(state, 1);
    const char* val = lh_checkstring(state, 2);
    aether_shared_map_put_by_token(current_map_token, key, val);
    return 0;
}

static void register_map_bindings(lua_State* state) {
    lh_pushcfunction(state, lua_aether_map_get);
    g_lua.lua_setglobal(state, "aether_map_get");
    lh_pushcfunction(state, lua_aether_map_put);
    g_lua.lua_setglobal(state, "aether_map_put");
}

int lua_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    if (lua_init() != 0) return -1;
    register_map_bindings(L);

    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);

    current_map_token = map_token;

    if (lua_perms_depth >= 64) return -1;

    lua_perms_stack[lua_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = host_lua_checker;

    int result = 0;
    if (lh_dostring(L, code) != 0) {
        fprintf(stderr, "[lua] %s\n", lh_tostring(L, -1));
        lh_pop(L, 1);
        result = -1;
    }

    _aether_sandbox_checker = prev;
    lua_perms_depth--;
    current_map_token = 0;

    return result;
}

// === #904 bidirectional embedding API ======================================
//
// A Redis-class host (aedis EVAL/FCALL) needs more than fire-and-forget: a
// persistent host-owned VM, host callbacks the script can call (which read a
// typed arg stack and build a typed return), typed eval results, injected
// globals, and an instruction-count guard. This block adds that surface on
// top of the dlsym table above. The Aether-facing names are `lua_vm_*` /
// `lua_arg_*` / `lua_push_*` in module.ae.
//
// VM handle: an opaque `void*` that IS the lua_State* (the persistent VM the
// host owns; distinct from the single global `L` the fire-and-forget path
// uses). Callers pass it back to every lua_vm_* call.
//
// Typed value tags (must match module.ae's constants):
#define AE_LUA_TNIL    0
#define AE_LUA_TBOOL   1
#define AE_LUA_TINT    2
#define AE_LUA_TSTR    3
#define AE_LUA_TTABLE  4
#define AE_LUA_TERR    5
#define AE_LUA_TOTHER  6

/* Lua's own LUA_T* constants (stable across 5.3/5.4). */
#define AE_LT_NIL      0
#define AE_LT_BOOLEAN  1
#define AE_LT_NUMBER   3
#define AE_LT_STRING   4
#define AE_LT_TABLE    5

/* A registered host callback: an Aether fn `(vm: ptr) -> int` that reads args
 * via lua_arg_* and pushes its result via lua_push_*, returning the number of
 * results it pushed (the Lua C-function convention). We carry the host fn
 * pointer through a Lua C-closure upvalue (light userdata) so one trampoline
 * serves every registration. */
typedef int (*ae_lua_host_fn)(void* vm);

/* The C trampoline installed as the Lua C-function. It recovers the host fn
 * pointer from upvalue 1 and calls it with the lua_State as the opaque vm. */
static int ae_lua_trampoline(lua_State* state) {
    /* upvalueindex(1) == LUA_REGISTRYINDEX-… ; the portable spelling is
     * lua_upvalueindex(1), a macro = (LUA_REGISTRYINDEX - n). We compute it
     * without the macro: LUA_REGISTRYINDEX is -1001000 on 5.3/5.4. */
    const int LUA_REGISTRYINDEX_V = -1001000;
    void* p = g_lua.lua_touserdata(state, LUA_REGISTRYINDEX_V - 1);
    if (!p) return 0;
    ae_lua_host_fn fn = (ae_lua_host_fn)p;
    return fn((void*)state);
}

/* Create a persistent, host-owned VM. Returns the handle (lua_State*) or NULL.
 * libs are opened; the sandbox checker is NOT installed here (the host drives
 * it per eval via lua_vm_eval_sandboxed if it wants the libc gate). */
void* lua_vm_new(void) {
    if (load_liblua() != 0) return NULL;
    lua_State* vm = g_lua.luaL_newstate();
    if (!vm) return NULL;
    g_lua.luaL_openlibs(vm);
    return (void*)vm;
}

void lua_vm_free(void* vm) {
    if (vm) g_lua.lua_close((lua_State*)vm);
}

/* Register a host callback as a Lua global named `name` (may be dotted, e.g.
 * "redis.call": we create/reuse the `redis` table and set the field). The
 * Aether fn is passed as a raw ptr (its address). Returns 0 on success. */
int lua_vm_register(void* vm, const char* name, void* host_fn) {
    if (!vm || !name || !host_fn) return -1;
    lua_State* s = (lua_State*)vm;
    const char* dot = strchr(name, '.');
    /* Build the C closure: push the host fn ptr as an upvalue, then the
     * trampoline as a 1-upvalue C closure. */
    g_lua.lua_pushlightuserdata(s, host_fn);
    g_lua.lua_pushcclosure(s, ae_lua_trampoline, 1);
    if (!dot) {
        g_lua.lua_setglobal(s, name);
        return 0;
    }
    /* dotted: ensure global table `<prefix>` exists, set `<field>` on it. */
    char prefix[128];
    size_t plen = (size_t)(dot - name);
    if (plen >= sizeof(prefix)) { g_lua.lua_settop(s, -2); return -1; }
    memcpy(prefix, name, plen); prefix[plen] = '\0';
    const char* field = dot + 1;
    /* stack: [closure]. Get or create the prefix table. */
    int t = g_lua.lua_getglobal(s, prefix);            /* [closure, tbl|nil] */
    if (t != AE_LT_TABLE) {
        g_lua.lua_settop(s, -2);                        /* pop nil → [closure] */
        g_lua.lua_createtable(s, 0, 4);                 /* [closure, tbl] */
        g_lua.lua_pushvalue(s, -1);                     /* [closure, tbl, tbl] */
        g_lua.lua_setglobal(s, prefix);                 /* [closure, tbl] */
    }
    /* stack: [closure, tbl]. setfield(tbl, field) consumes the closure —
     * but the closure is below tbl; move tbl under closure by pushing a copy
     * arrangement. Simpler: re-fetch. We have [closure, tbl]; we need
     * tbl.field = closure. lua_setfield(idx=-2 → tbl) pops the value at -1
     * (must be the closure). Swap so closure is on top: */
    g_lua.lua_pushvalue(s, -2);                         /* [closure, tbl, closure] */
    g_lua.lua_setfield(s, -2, field);                   /* tbl.field=closure → [closure, tbl] */
    g_lua.lua_settop(s, -3);                            /* pop tbl, closure */
    return 0;
}

/* Inject globals before a run. */
void lua_vm_set_global_str(void* vm, const char* name, const char* value) {
    if (!vm || !name) return;
    lua_State* s = (lua_State*)vm;
    g_lua.lua_pushstring(s, value ? value : "");
    g_lua.lua_setglobal(s, name);
}

void lua_vm_set_global_int(void* vm, const char* name, long value) {
    if (!vm || !name) return;
    lua_State* s = (lua_State*)vm;
    g_lua.lua_pushinteger(s, (lua_Integer)value);
    g_lua.lua_setglobal(s, name);
}

/* Set a global to a Lua array (table with 1..n string elements) from an
 * Aether list of strings (the KEYS/ARGV shape). `items` is an Aether list
 * handle; we read it with the existing list_size/list_get_raw externs. */
void lua_vm_set_global_strlist(void* vm, const char* name, void* items) {
    if (!vm || !name) return;
    lua_State* s = (lua_State*)vm;
    int n = items ? list_size(items) : 0;
    g_lua.lua_createtable(s, n, 0);
    for (int i = 0; i < n; i++) {
        const char* v = aether_string_data(list_get_raw(items, i));
        g_lua.lua_pushstring(s, v ? v : "");
        g_lua.lua_seti(s, -2, (lua_Integer)(i + 1));   /* 1-based Lua array */
    }
    g_lua.lua_setglobal(s, name);
}

/* Run `code` in the VM, leaving its (single) return value on the stack for the
 * lua_vm_result_* readers. Returns 0 on success, -1 on error (the error
 * object/string is left on the stack and readable as type AE_LUA_TERR). */
int lua_vm_eval(void* vm, const char* code) {
    if (!vm || !code) return -1;
    lua_State* s = (lua_State*)vm;
    if (g_lua.luaL_loadstring(s, code) != 0) return -1;    /* compile error on stack */
    if (g_lua.lua_pcallk(s, 0, 1, 0, 0, NULL) != 0) return -1; /* runtime error on stack */
    return 0;
}

/* Map the top-of-stack (or any index) Lua type to an AE_LUA_T* tag. */
static int ae_lua_tag_at(lua_State* s, int idx) {
    switch (g_lua.lua_type(s, idx)) {
        case AE_LT_NIL:     return AE_LUA_TNIL;
        case AE_LT_BOOLEAN: return AE_LUA_TBOOL;
        case AE_LT_NUMBER:  return AE_LUA_TINT;
        case AE_LT_STRING:  return AE_LUA_TSTR;
        case AE_LT_TABLE:   return AE_LUA_TTABLE;
        default:            return AE_LUA_TOTHER;
    }
}

/* Typed top-of-stack readers (the eval result, or a callback arg via index). */
int lua_vm_result_type(void* vm) {
    if (!vm) return AE_LUA_TNIL;
    return ae_lua_tag_at((lua_State*)vm, -1);
}
long lua_vm_result_int(void* vm) {
    if (!vm) return 0;
    int isnum = 0;
    return (long)g_lua.lua_tointegerx((lua_State*)vm, -1, &isnum);
}
const char* lua_vm_result_str(void* vm) {
    if (!vm) return "";
    const char* r = g_lua.lua_tolstring((lua_State*)vm, -1, NULL);
    return r ? r : "";
}
int lua_vm_result_bool(void* vm) {
    if (!vm) return 0;
    return g_lua.lua_toboolean((lua_State*)vm, -1);
}
/* Drop the result (and anything above base) — call after reading. */
void lua_vm_pop_result(void* vm) {
    if (vm) g_lua.lua_settop((lua_State*)vm, 0);
}

// --- callback-side stack API (used INSIDE a registered host fn) ------------
// Within `host_fn(vm)`, args are at stack indices 1..N; results are pushed
// and the fn returns the count.
int lua_arg_count(void* vm) {
    if (!vm) return 0;
    return g_lua.lua_gettop((lua_State*)vm);
}
int lua_arg_type(void* vm, int i) {
    if (!vm) return AE_LUA_TNIL;
    return ae_lua_tag_at((lua_State*)vm, i);
}
const char* lua_arg_str(void* vm, int i) {
    if (!vm) return "";
    const char* r = g_lua.lua_tolstring((lua_State*)vm, i, NULL);
    return r ? r : "";
}
long lua_arg_int(void* vm, int i) {
    if (!vm) return 0;
    int isnum = 0;
    return (long)g_lua.lua_tointegerx((lua_State*)vm, i, &isnum);
}
int lua_arg_bool(void* vm, int i) {
    if (!vm) return 0;
    return g_lua.lua_toboolean((lua_State*)vm, i);
}
void lua_push_int(void* vm, long v) {
    if (vm) g_lua.lua_pushinteger((lua_State*)vm, (lua_Integer)v);
}
void lua_push_str(void* vm, const char* v) {
    if (vm) g_lua.lua_pushstring((lua_State*)vm, v ? v : "");
}
void lua_push_bool(void* vm, int v) {
    if (vm) g_lua.lua_pushboolean((lua_State*)vm, v);
}
void lua_push_nil(void* vm) {
    if (vm) g_lua.lua_pushnil((lua_State*)vm);
}
/* Push a Redis-style { ok = msg } / { err = msg } single-field table — the
 * status/error-reply convention. `field` is "ok" or "err". */
void lua_push_tagged_table(void* vm, const char* field, const char* msg) {
    if (!vm || !field) return;
    lua_State* s = (lua_State*)vm;
    g_lua.lua_createtable(s, 0, 1);
    g_lua.lua_pushstring(s, msg ? msg : "");
    g_lua.lua_setfield(s, -2, field);
}
/* Raise a Lua error with `msg` as the error object (does not return in Lua
 * semantics; the host fn should `return lua_raise_error(vm, m)` so control
 * leaves via longjmp). */
int lua_raise_error(void* vm, const char* msg) {
    if (!vm) return 0;
    lua_State* s = (lua_State*)vm;
    g_lua.lua_pushstring(s, msg ? msg : "error");
    return g_lua.lua_error(s);   /* longjmps; never returns */
}

// === #910 host-callback table builder ======================================
//
// A host callback can push scalars and a single {ok}/{err} table, but a
// faithful `redis.call` returns RESP multibulks — arrays / maps / nested
// tables (`KEYS`, `LRANGE`, `HGETALL`, `SMEMBERS`, array-of-arrays). This
// builder exposes the same lua_createtable + lua_seti/setfield machinery the
// global-injection path already uses, for callback RETURNS.
//
// Model: "the table being built is on top of the stack." table_begin pushes a
// fresh table; the table_set* ops write into the top table; table_begin can be
// nested (push a child table), and table_end_seti / table_end_setfield store
// the finished child into the parent (now exposed at the top) at an index/key
// and pop back to the parent. A small depth tracker guards underflow.
//
// Typical use, building {"k1","k2"} as a callback return:
//   lua_table_begin(vm, 2, 0);
//   lua_table_seti_str(vm, 1, "k1");
//   lua_table_seti_str(vm, 2, "k2");
//   return 1;   // the table is the single return value, left on the stack
//
// Nested (array-of-arrays) — outer[1] = {"a"}:
//   lua_table_begin(vm, 1, 0);        // outer
//   lua_table_begin(vm, 1, 0);        // inner (child, on top)
//   lua_table_seti_str(vm, 1, "a");
//   lua_table_end_seti(vm, 1);        // outer[1] = inner; back to outer on top
//   return 1;

#define AE_LUA_TBL_MAXDEPTH 32
static int g_tbl_depth = 0;   /* per-process; the EVAL model is single-threaded */

/* Push a fresh table; it becomes the top-of-stack "current" table. */
void lua_table_begin(void* vm, int narr, int nrec) {
    if (!vm) return;
    if (g_tbl_depth >= AE_LUA_TBL_MAXDEPTH) return;  /* guard runaway nesting */
    g_lua.lua_createtable((lua_State*)vm, narr < 0 ? 0 : narr, nrec < 0 ? 0 : nrec);
    g_tbl_depth++;
}

/* Array sets: current_table[i] = value  (1-based, Lua convention). */
void lua_table_seti_str(void* vm, int i, const char* v) {
    if (!vm || g_tbl_depth <= 0) return;
    lua_State* s = (lua_State*)vm;
    g_lua.lua_pushstring(s, v ? v : "");
    g_lua.lua_seti(s, -2, (lua_Integer)i);
}
void lua_table_seti_int(void* vm, int i, long v) {
    if (!vm || g_tbl_depth <= 0) return;
    lua_State* s = (lua_State*)vm;
    g_lua.lua_pushinteger(s, (lua_Integer)v);
    g_lua.lua_seti(s, -2, (lua_Integer)i);
}
void lua_table_seti_bool(void* vm, int i, int v) {
    if (!vm || g_tbl_depth <= 0) return;
    lua_State* s = (lua_State*)vm;
    g_lua.lua_pushboolean(s, v);
    g_lua.lua_seti(s, -2, (lua_Integer)i);
}

/* Map sets: current_table[key] = value  (string keys, RESP3 map/hash shape). */
void lua_table_set_str(void* vm, const char* key, const char* v) {
    if (!vm || g_tbl_depth <= 0 || !key) return;
    lua_State* s = (lua_State*)vm;
    g_lua.lua_pushstring(s, v ? v : "");
    g_lua.lua_setfield(s, -2, key);
}
void lua_table_set_int(void* vm, const char* key, long v) {
    if (!vm || g_tbl_depth <= 0 || !key) return;
    lua_State* s = (lua_State*)vm;
    g_lua.lua_pushinteger(s, (lua_Integer)v);
    g_lua.lua_setfield(s, -2, key);
}
void lua_table_set_bool(void* vm, const char* key, int v) {
    if (!vm || g_tbl_depth <= 0 || !key) return;
    lua_State* s = (lua_State*)vm;
    g_lua.lua_pushboolean(s, v);
    g_lua.lua_setfield(s, -2, key);
}

/* Close the current (child) table by storing it into the now-exposed parent
 * table at array index `i`, then leaving the parent on top. Requires at least
 * two open tables (a parent and the child). */
void lua_table_end_seti(void* vm, int i) {
    if (!vm || g_tbl_depth < 2) return;   /* need parent + child */
    lua_State* s = (lua_State*)vm;
    /* stack: [..., parent, child]. Store child into parent[i]; lua_seti pops
     * the value (the child), leaving the parent on top. */
    g_lua.lua_seti(s, -2, (lua_Integer)i);
    g_tbl_depth--;
}
/* Close the current (child) table into the parent at string key `key`. */
void lua_table_end_setfield(void* vm, const char* key) {
    if (!vm || g_tbl_depth < 2 || !key) return;
    lua_State* s = (lua_State*)vm;
    g_lua.lua_setfield(s, -2, key);
    g_tbl_depth--;
}
/* Finish the OUTERMOST table: it stays on the stack as the callback's return
 * value. Just resets the depth tracker (the table is already on top). Call
 * once, matching the first lua_table_begin, then `return 1`. */
void lua_table_finish(void* vm) {
    (void)vm;
    if (g_tbl_depth > 0) g_tbl_depth--;
}

// --- RESP3 typed scalars the callback can also return ----------------------
/* Lua has one number type (5.3+ distinguishes int/float subtypes); a RESP3
 * Double marshals to a Lua float. We push via the number path. */
void lua_push_double(void* vm, double v) {
    if (!vm) return;
    /* lua_pushnumber isn't in the dlsym table; push the textual form is wrong
     * for a Double. Use lua_pushinteger's sibling only if integral; otherwise
     * fall back to a string so no precision is silently lost on 5.3 where the
     * float push isn't resolved. Most RESP Doubles arrive integral. */
    lua_State* s = (lua_State*)vm;
    double r = v < 0 ? -v : v;
    if (r == (double)(long)v) {
        g_lua.lua_pushinteger(s, (lua_Integer)(long)v);
    } else {
        char buf[40];
        snprintf(buf, sizeof(buf), "%.17g", v);
        g_lua.lua_pushstring(s, buf);
    }
}

// --- execution guard: instruction-count hook -------------------------------
// The host arms a count hook; when the budget is hit the hook raises a Lua
// error (the script-timeout shape). One global budget per process is enough
// for the single-threaded EVAL model; a richer per-VM budget can follow.
static long g_lua_instr_budget = 0;
static long g_lua_instr_used = 0;

static void ae_lua_count_hook(lua_State* s, lua_Debug* ar) {
    (void)ar;
    g_lua_instr_used += 1;
    if (g_lua_instr_budget > 0 && g_lua_instr_used >= g_lua_instr_budget) {
        g_lua.lua_pushstring(s, "script exceeded instruction budget");
        g_lua.lua_error(s);
    }
}

/* Arm the count hook: fire every `every` VM instructions, abort after
 * `budget` total. `budget`<=0 disarms. LUA_MASKCOUNT == 1<<3 == 8. */
void lua_vm_set_instruction_limit(void* vm, long budget, int every) {
    if (!vm) return;
    lua_State* s = (lua_State*)vm;
    g_lua_instr_budget = budget;
    g_lua_instr_used = 0;
    if (budget > 0) {
        g_lua.lua_sethook(s, ae_lua_count_hook, 8 /*LUA_MASKCOUNT*/,
                          every > 0 ? every : 1);
    } else {
        g_lua.lua_sethook(s, NULL, 0, 0);
    }
}

#else
#include <stdio.h>
int lua_init(void) {
    fprintf(stderr, "error: contrib.host.lua not available (compile with AETHER_HAS_LUA)\n");
    return -1;
}
void lua_finalize(void) {}
int lua_run(const char* code) { (void)code; return lua_init(); }
int lua_run_sandboxed(void* perms, const char* code) { (void)perms; (void)code; return lua_init(); }
int lua_run_sandboxed_with_map(void* perms, const char* code,
    uint64_t token) {
  (void)perms; (void)code; (void)token;
  return lua_init();
}
// #904 bidirectional API — unavailable stubs.
void* lua_vm_new(void) { (void)lua_init(); return 0; }
void  lua_vm_free(void* vm) { (void)vm; }
int   lua_vm_register(void* vm, const char* n, void* f) { (void)vm; (void)n; (void)f; return -1; }
void  lua_vm_set_global_str(void* vm, const char* n, const char* v) { (void)vm; (void)n; (void)v; }
void  lua_vm_set_global_int(void* vm, const char* n, long v) { (void)vm; (void)n; (void)v; }
void  lua_vm_set_global_strlist(void* vm, const char* n, void* it) { (void)vm; (void)n; (void)it; }
int   lua_vm_eval(void* vm, const char* c) { (void)vm; (void)c; return -1; }
int   lua_vm_result_type(void* vm) { (void)vm; return 0; }
long  lua_vm_result_int(void* vm) { (void)vm; return 0; }
const char* lua_vm_result_str(void* vm) { (void)vm; return ""; }
int   lua_vm_result_bool(void* vm) { (void)vm; return 0; }
void  lua_vm_pop_result(void* vm) { (void)vm; }
int   lua_arg_count(void* vm) { (void)vm; return 0; }
int   lua_arg_type(void* vm, int i) { (void)vm; (void)i; return 0; }
const char* lua_arg_str(void* vm, int i) { (void)vm; (void)i; return ""; }
long  lua_arg_int(void* vm, int i) { (void)vm; (void)i; return 0; }
int   lua_arg_bool(void* vm, int i) { (void)vm; (void)i; return 0; }
void  lua_push_int(void* vm, long v) { (void)vm; (void)v; }
void  lua_push_str(void* vm, const char* v) { (void)vm; (void)v; }
void  lua_push_bool(void* vm, int v) { (void)vm; (void)v; }
void  lua_push_nil(void* vm) { (void)vm; }
void  lua_push_tagged_table(void* vm, const char* f, const char* m) { (void)vm; (void)f; (void)m; }
int   lua_raise_error(void* vm, const char* m) { (void)vm; (void)m; return 0; }
void  lua_vm_set_instruction_limit(void* vm, long b, int e) { (void)vm; (void)b; (void)e; }
// #910 table builder — unavailable stubs.
void  lua_table_begin(void* vm, int a, int r) { (void)vm; (void)a; (void)r; }
void  lua_table_seti_str(void* vm, int i, const char* v) { (void)vm; (void)i; (void)v; }
void  lua_table_seti_int(void* vm, int i, long v) { (void)vm; (void)i; (void)v; }
void  lua_table_seti_bool(void* vm, int i, int v) { (void)vm; (void)i; (void)v; }
void  lua_table_set_str(void* vm, const char* k, const char* v) { (void)vm; (void)k; (void)v; }
void  lua_table_set_int(void* vm, const char* k, long v) { (void)vm; (void)k; (void)v; }
void  lua_table_set_bool(void* vm, const char* k, int v) { (void)vm; (void)k; (void)v; }
void  lua_table_end_seti(void* vm, int i) { (void)vm; (void)i; }
void  lua_table_end_setfield(void* vm, const char* k) { (void)vm; (void)k; }
void  lua_table_finish(void* vm) { (void)vm; }
void  lua_push_double(void* vm, double v) { (void)vm; (void)v; }
#endif
