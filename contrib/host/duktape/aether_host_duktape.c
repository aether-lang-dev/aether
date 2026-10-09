// aether_host_duktape.c — Embedded JavaScript Language Host Module (Duktape)
//
// Unlike Python/Lua, Duktape has NO built-in filesystem or env access.
// We provide native bindings (env, readFile, print) that go through
// the Aether sandbox checker. This is the purest containment model —
// the guest can ONLY do what we explicitly expose.
//
// LOADING MODEL: dlopen, not -lduktape. Mirrors contrib/host/python,
// ruby, lua, perl — every Duktape C-API symbol resolved via dlsym at
// first call. The bridge .a has NO unresolved Duktape symbols at
// link time, so end-user binaries have no DT_NEEDED libduktape and
// are ABI-portable across deploy-host Duktape minor versions.
//
// Duktape's headers `#define` a handful of convenience macros over
// the real exported functions:
//   duk_create_heap_default() → duk_create_heap(NULL,NULL,NULL,NULL,NULL)
//   duk_peval_string(c,s)     → duk_eval_raw(c,s,0,FLAGS)
//   duk_safe_to_string(c,i)   → duk_safe_to_lstring(c,i,NULL)
// We dlsym the wrapped functions and re-implement the macros as
// inline static helpers. The DUK_COMPILE_* flag constants are baked
// directly (public ABI of duktape's compile-flags interface).

#include "aether_host_duktape.h"
#include "../../../runtime/aether_sandbox.h"
#include "../../../runtime/aether_shared_map.h"

#ifdef AETHER_HAS_DUKTAPE
#include <duktape.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

// --- libduktape dlopen table ------------------------------------------------

static void* libduktape_handle = NULL;

// duk_peval_string flag set (documented public DUK_COMPILE_* constants):
//   EVAL (1<<3) | SAFE (1<<7) | NOSOURCE (1<<9) | STRLEN (1<<10) | NOFILENAME (1<<11)
#define BRIDGE_DUK_PEVAL_FLAGS \
    (DUK_COMPILE_EVAL | DUK_COMPILE_SAFE | DUK_COMPILE_NOSOURCE | \
     DUK_COMPILE_STRLEN | DUK_COMPILE_NOFILENAME)

static struct {
    // Heap lifecycle.
    duk_context* (*duk_create_heap)(duk_alloc_function alloc_func,
                                    duk_realloc_function realloc_func,
                                    duk_free_function free_func,
                                    void* heap_udata,
                                    duk_fatal_function fatal_handler);
    void (*duk_destroy_heap)(duk_context* ctx);
    // Eval (real fn behind duk_peval_string macro).
    duk_int_t (*duk_eval_raw)(duk_context* ctx, const char* src_buffer,
                              duk_size_t src_length, duk_uint_t flags);
    // String coercion (real fn behind duk_safe_to_string).
    const char* (*duk_safe_to_lstring)(duk_context* ctx, duk_idx_t idx,
                                       duk_size_t* out_len);
    // Stack manipulation.
    duk_idx_t (*duk_get_top)(duk_context* ctx);
    void (*duk_pop)(duk_context* ctx);
    // Value pushers.
    const char* (*duk_push_string)(duk_context* ctx, const char* str);
    void (*duk_push_int)(duk_context* ctx, duk_int_t val);
    void (*duk_push_boolean)(duk_context* ctx, duk_bool_t val);
    void (*duk_push_undefined)(duk_context* ctx);
    duk_idx_t (*duk_push_c_function)(duk_context* ctx,
                                     duk_c_function func, duk_idx_t nargs);
    // Value getters.
    const char* (*duk_require_string)(duk_context* ctx, duk_idx_t idx);
    const char* (*duk_require_lstring)(duk_context* ctx, duk_idx_t idx,
                                       duk_size_t* out_len);
    const char* (*duk_to_string)(duk_context* ctx, duk_idx_t idx);
    // Globals.
    duk_bool_t (*duk_put_global_string)(duk_context* ctx, const char* key);
} g_duk;

static int resolve_duktape_symbols(void* h) {
#define RESOLVE(field, sym) do {                                       \
        *(void**)(&g_duk.field) = dlsym(h, sym);                       \
        if (!g_duk.field) {                                            \
            fprintf(stderr,                                            \
                "aether host_duktape: libduktape missing symbol %s\n", sym);\
            return -1;                                                 \
        }                                                              \
    } while (0)

    RESOLVE(duk_create_heap,        "duk_create_heap");
    RESOLVE(duk_destroy_heap,       "duk_destroy_heap");
    RESOLVE(duk_eval_raw,           "duk_eval_raw");
    RESOLVE(duk_safe_to_lstring,    "duk_safe_to_lstring");
    RESOLVE(duk_get_top,            "duk_get_top");
    RESOLVE(duk_pop,                "duk_pop");
    RESOLVE(duk_push_string,        "duk_push_string");
    RESOLVE(duk_push_int,           "duk_push_int");
    RESOLVE(duk_push_boolean,       "duk_push_boolean");
    RESOLVE(duk_push_undefined,     "duk_push_undefined");
    RESOLVE(duk_push_c_function,    "duk_push_c_function");
    RESOLVE(duk_require_string,     "duk_require_string");
    RESOLVE(duk_require_lstring,    "duk_require_lstring");
    RESOLVE(duk_to_string,          "duk_to_string");
    RESOLVE(duk_put_global_string,  "duk_put_global_string");
    return 0;
#undef RESOLVE
}

static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

// Load libduktape on first use. Strict two-step contract:
//   1. ${AETHER_DUKTAPE_SONAME} env var (orchestrator-supplied exact,
//      e.g. "libduktape.so.207"). Duktape is embedded-only (no
//      `duktape` CLI to probe with), so the orchestrator may need
//      to find the soname via `ldconfig -p | grep libduktape` or
//      similar package-manager-specific probes.
//   2. libduktape.so (Debian-style unversioned symlink).
// No hardcoded version-list fallback. Orchestrator owns the probe.
static int load_libduktape(void) {
    if (libduktape_handle) return 0;

    const char* env_soname = getenv("AETHER_DUKTAPE_SONAME");
    void* h = try_dlopen(env_soname);
    if (!h) h = try_dlopen("libduktape.so");
    if (!h) {
        fprintf(stderr,
            "aether host_duktape: cannot dlopen libduktape "
            "(tried $AETHER_DUKTAPE_SONAME=%s, libduktape.so).\n"
            "  Install duktape on the host, or set AETHER_DUKTAPE_SONAME "
            "to the exact soname.\n"
            "  Hint: $(ldconfig -p | awk '/libduktape\\.so/{print $1; exit}')\n"
            "  dlerror: %s\n",
            env_soname ? env_soname : "(unset)",
            dlerror() ? dlerror() : "(none)");
        return -1;
    }

    if (resolve_duktape_symbols(h) != 0) {
        dlclose(h);
        return -1;
    }
    libduktape_handle = h;
    return 0;
}

// --- macro re-expressions --------------------------------------------------

static duk_context* dh_create_heap_default(void) {
    return g_duk.duk_create_heap(NULL, NULL, NULL, NULL, NULL);
}

static duk_int_t dh_peval_string(duk_context* c, const char* src) {
    return g_duk.duk_eval_raw(c, src, 0, BRIDGE_DUK_PEVAL_FLAGS);
}

static const char* dh_safe_to_string(duk_context* c, duk_idx_t idx) {
    return g_duk.duk_safe_to_lstring(c, idx, NULL);
}

// --- bridge state (unchanged) ----------------------------------------------

static duk_context* ctx = NULL;

static void* duktape_perms_stack[64];
static int   duktape_perms_depth = 0;

extern int list_size(void*);
extern void* list_get_raw(void*, int);
/* A grant list std.sandbox built holds plain C strings; one a program
 * built can hold Aether strings (a helper that keeps its `string`
 * parameter stores its own copy). This reads the bytes of either. */
extern const char* aether_string_data(const void*);

// The shared grant matcher (runtime/aether_sandbox_match.h).
#include "aether_sandbox_match.h"

static int perms_allow(void* perms, const char* category, const char* resource) {
    if (!perms) return 1;
    int n = list_size(perms);
    if (n == 0) return 0;
    for (int i = 0; i < n; i += 2) {
        const char* cat = aether_string_data(list_get_raw(perms, i));
        const char* pat = aether_string_data(list_get_raw(perms, i + 1));
        if (!cat || !pat) continue;
        if (cat[0] == '*' && pat[0] == '*') return 1;
        if (strcmp(cat, category) == 0 && aether_grant_match(category, pat, resource)) return 1;
    }
    return 0;
}

static int check_sandbox(const char* category, const char* resource) {
    if (duktape_perms_depth <= 0) return 1;
    for (int level = 0; level < duktape_perms_depth; level++) {
        if (!perms_allow(duktape_perms_stack[level], category, resource)) return 0;
    }
    return 1;
}

// --- Duktape native bindings (exposed to JS) -------------------------------

static duk_ret_t duktape_print(duk_context* c) {
    int n = g_duk.duk_get_top(c);
    for (int i = 0; i < n; i++) {
        if (i > 0) printf(" ");
        printf("%s", g_duk.duk_to_string(c, i));
    }
    printf("\n");
    return 0;
}

static duk_ret_t duktape_env(duk_context* c) {
    const char* name = g_duk.duk_require_string(c, 0);
    if (!check_sandbox("env", name)) {
        g_duk.duk_push_undefined(c);
        return 1;
    }
    const char* val = getenv(name);
    if (val) {
        g_duk.duk_push_string(c, val);
    } else {
        g_duk.duk_push_undefined(c);
    }
    return 1;
}

static duk_ret_t duktape_read_file(duk_context* c) {
    const char* path = g_duk.duk_require_string(c, 0);
    if (!check_sandbox("fs_read", path)) {
        g_duk.duk_push_undefined(c);
        return 1;
    }
    FILE* f = fopen(path, "r");
    if (!f) {
        g_duk.duk_push_undefined(c);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = malloc(len + 1);
    if (!buf) { fclose(f); g_duk.duk_push_undefined(c); return 1; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    g_duk.duk_push_string(c, buf);
    free(buf);
    return 1;
}

static duk_ret_t duktape_file_exists(duk_context* c) {
    const char* path = g_duk.duk_require_string(c, 0);
    if (!check_sandbox("fs_read", path)) {
        g_duk.duk_push_boolean(c, 0);
        return 1;
    }
    FILE* f = fopen(path, "r");
    if (f) { fclose(f); g_duk.duk_push_boolean(c, 1); }
    else { g_duk.duk_push_boolean(c, 0); }
    return 1;
}

static duk_ret_t duktape_write_file(duk_context* c) {
    const char* path = g_duk.duk_require_string(c, 0);
    duk_size_t len = 0;
    const char* content = g_duk.duk_require_lstring(c, 1, &len);
    if (!check_sandbox("fs_write", path)) {
        g_duk.duk_push_boolean(c, 0);
        return 1;
    }
    FILE* f = fopen(path, "w");
    if (!f) { g_duk.duk_push_boolean(c, 0); return 1; }
    size_t written = fwrite(content, 1, len, f);
    fclose(f);
    g_duk.duk_push_boolean(c, written == len);
    return 1;
}

static duk_ret_t duktape_exec(duk_context* c) {
    const char* cmd = g_duk.duk_require_string(c, 0);
    if (!check_sandbox("exec", cmd)) {
        g_duk.duk_push_int(c, -1);
        return 1;
    }
    int rc = system(cmd);
#ifdef WEXITSTATUS
    if (rc >= 0) rc = WEXITSTATUS(rc);
#endif
    g_duk.duk_push_int(c, rc);
    return 1;
}

static void register_bindings(duk_context* c) {
    g_duk.duk_push_c_function(c, duktape_print, DUK_VARARGS);
    g_duk.duk_put_global_string(c, "print");

    g_duk.duk_push_c_function(c, duktape_env, 1);
    g_duk.duk_put_global_string(c, "env");

    g_duk.duk_push_c_function(c, duktape_read_file, 1);
    g_duk.duk_put_global_string(c, "readFile");

    g_duk.duk_push_c_function(c, duktape_file_exists, 1);
    g_duk.duk_put_global_string(c, "fileExists");

    g_duk.duk_push_c_function(c, duktape_write_file, 2);
    g_duk.duk_put_global_string(c, "writeFile");

    g_duk.duk_push_c_function(c, duktape_exec, 1);
    g_duk.duk_put_global_string(c, "exec");
}

int duktape_init(void) {
    if (ctx) return 0;
    if (load_libduktape() != 0) return -1;
    ctx = dh_create_heap_default();
    if (!ctx) return -1;
    register_bindings(ctx);
    return 0;
}

void duktape_finalize(void) {
    if (ctx) {
        g_duk.duk_destroy_heap(ctx);
        ctx = NULL;
    }
}

int duktape_run(const char* code) {
    if (!code) return -1;
    if (duktape_init() != 0) return -1;
    if (dh_peval_string(ctx, code) != 0) {
        fprintf(stderr, "[js] %s\n", dh_safe_to_string(ctx, -1));
        g_duk.duk_pop(ctx);
        return -1;
    }
    g_duk.duk_pop(ctx);
    return 0;
}

int duktape_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (duktape_init() != 0) return -1;

    if (duktape_perms_depth >= 64) return -1;
    duktape_perms_stack[duktape_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = NULL;  // JS uses direct check_sandbox, not libc interception

    int result = 0;
    if (dh_peval_string(ctx, code) != 0) {
        fprintf(stderr, "[js] %s\n", dh_safe_to_string(ctx, -1));
        g_duk.duk_pop(ctx);
        result = -1;
    } else {
        g_duk.duk_pop(ctx);
    }

    _aether_sandbox_checker = prev;
    duktape_perms_depth--;

    return result;
}

// --- Shared map bindings for JS --------------------------------------------

static uint64_t duktape_current_map_token = 0;

static duk_ret_t duktape_aether_map_get(duk_context* c) {
    const char* key = g_duk.duk_require_string(c, 0);
    const char* val = aether_shared_map_get_by_token(duktape_current_map_token, key);
    if (val) { g_duk.duk_push_string(c, val); } else { g_duk.duk_push_undefined(c); }
    return 1;
}

static duk_ret_t duktape_aether_map_put(duk_context* c) {
    const char* key = g_duk.duk_require_string(c, 0);
    const char* val = g_duk.duk_require_string(c, 1);
    aether_shared_map_put_by_token(duktape_current_map_token, key, val);
    return 0;
}

static void register_map_bindings(duk_context* c) {
    g_duk.duk_push_c_function(c, duktape_aether_map_get, 1);
    g_duk.duk_put_global_string(c, "aether_map_get");
    g_duk.duk_push_c_function(c, duktape_aether_map_put, 2);
    g_duk.duk_put_global_string(c, "aether_map_put");
}

int duktape_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    if (duktape_init() != 0) return -1;
    register_map_bindings(ctx);

    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);
    duktape_current_map_token = map_token;

    if (duktape_perms_depth >= 64) return -1;
    duktape_perms_stack[duktape_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = NULL;

    int result = 0;
    if (dh_peval_string(ctx, code) != 0) {
        fprintf(stderr, "[js] %s\n", dh_safe_to_string(ctx, -1));
        g_duk.duk_pop(ctx);
        result = -1;
    } else {
        g_duk.duk_pop(ctx);
    }

    _aether_sandbox_checker = prev;
    duktape_perms_depth--;
    duktape_current_map_token = 0;

    return result;
}

#else
#include <stdio.h>
int duktape_init(void) {
    fprintf(stderr, "error: contrib.host.duktape not available (compile with AETHER_HAS_DUKTAPE)\n");
    return -1;
}
void duktape_finalize(void) {}
int duktape_run(const char* code) { (void)code; return duktape_init(); }
int duktape_run_sandboxed(void* perms, const char* code) { (void)perms; (void)code; return duktape_init(); }
int duktape_run_sandboxed_with_map(void* perms, const char* code,
    uint64_t token) {
  (void)perms; (void)code; (void)token;
  return duktape_init();
}
#endif
