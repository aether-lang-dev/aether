// aether_host_duktape.c — Embedded JavaScript Language Host Module (Duktape)
//
// Unlike Python/Lua, Duktape has NO built-in filesystem or env access.
// We provide native bindings (env, readFile, print) that go through
// the Aether sandbox check of the calling thread. This is the purest containment model —
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

#define AE_DUK_TNIL   0
#define AE_DUK_TBOOL  1
#define AE_DUK_TINT   2
#define AE_DUK_TSTR   3
#define AE_DUK_TTABLE 4
#define AE_DUK_TERR   5
#define AE_DUK_TOTHER 6

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
    void (*duk_push_null)(duk_context* ctx);
    void (*duk_push_number)(duk_context* ctx, duk_double_t val);
    void (*duk_push_pointer)(duk_context* ctx, void* p);
    duk_idx_t (*duk_push_object)(duk_context* ctx);
    duk_idx_t (*duk_push_array)(duk_context* ctx);
    void (*duk_push_global_object)(duk_context* ctx);
    void (*duk_push_current_function)(duk_context* ctx);
    duk_idx_t (*duk_push_c_function)(duk_context* ctx,
                                     duk_c_function func, duk_idx_t nargs);
    duk_idx_t (*duk_push_error_object_raw)(duk_context* ctx, duk_errcode_t err_code,
                                           const char* filename, duk_int_t line,
                                           const char* fmt, ...);
    void (*duk_throw_raw)(duk_context* ctx);
    // Value getters.
    const char* (*duk_require_string)(duk_context* ctx, duk_idx_t idx);
    const char* (*duk_require_lstring)(duk_context* ctx, duk_idx_t idx,
                                       duk_size_t* out_len);
    const char* (*duk_to_string)(duk_context* ctx, duk_idx_t idx);
    const char* (*duk_get_string)(duk_context* ctx, duk_idx_t idx);
    duk_double_t (*duk_get_number)(duk_context* ctx, duk_idx_t idx);
    duk_bool_t (*duk_get_boolean)(duk_context* ctx, duk_idx_t idx);
    void* (*duk_get_pointer)(duk_context* ctx, duk_idx_t idx);
    duk_int_t (*duk_get_type)(duk_context* ctx, duk_idx_t idx);
    duk_errcode_t (*duk_get_error_code)(duk_context* ctx, duk_idx_t idx);
    // Globals and Properties.
    duk_bool_t (*duk_get_prop_string)(duk_context* ctx, duk_idx_t idx, const char* key);
    duk_bool_t (*duk_put_prop_string)(duk_context* ctx, duk_idx_t idx, const char* key);
    duk_bool_t (*duk_put_prop_index)(duk_context* ctx, duk_idx_t idx, duk_uarridx_t arr_idx);
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

    RESOLVE(duk_create_heap,           "duk_create_heap");
    RESOLVE(duk_destroy_heap,          "duk_destroy_heap");
    RESOLVE(duk_eval_raw,              "duk_eval_raw");
    RESOLVE(duk_safe_to_lstring,       "duk_safe_to_lstring");
    RESOLVE(duk_get_top,               "duk_get_top");
    RESOLVE(duk_pop,                   "duk_pop");
    RESOLVE(duk_push_string,           "duk_push_string");
    RESOLVE(duk_push_int,              "duk_push_int");
    RESOLVE(duk_push_boolean,          "duk_push_boolean");
    RESOLVE(duk_push_undefined,        "duk_push_undefined");
    RESOLVE(duk_push_null,             "duk_push_null");
    RESOLVE(duk_push_number,           "duk_push_number");
    RESOLVE(duk_push_pointer,          "duk_push_pointer");
    RESOLVE(duk_push_object,           "duk_push_object");
    RESOLVE(duk_push_array,            "duk_push_array");
    RESOLVE(duk_push_global_object,    "duk_push_global_object");
    RESOLVE(duk_push_current_function, "duk_push_current_function");
    RESOLVE(duk_push_c_function,       "duk_push_c_function");
    RESOLVE(duk_push_error_object_raw, "duk_push_error_object_raw");
    RESOLVE(duk_throw_raw,             "duk_throw_raw");
    RESOLVE(duk_require_string,        "duk_require_string");
    RESOLVE(duk_require_lstring,       "duk_require_lstring");
    RESOLVE(duk_to_string,             "duk_to_string");
    RESOLVE(duk_get_string,            "duk_get_string");
    RESOLVE(duk_get_number,            "duk_get_number");
    RESOLVE(duk_get_boolean,           "duk_get_boolean");
    RESOLVE(duk_get_pointer,           "duk_get_pointer");
    RESOLVE(duk_get_type,              "duk_get_type");
    RESOLVE(duk_get_error_code,        "duk_get_error_code");
    RESOLVE(duk_get_prop_string,       "duk_get_prop_string");
    RESOLVE(duk_put_prop_string,       "duk_put_prop_string");
    RESOLVE(duk_put_prop_index,        "duk_put_prop_index");
    RESOLVE(duk_put_global_string,     "duk_put_global_string");
    return 0;
#undef RESOLVE
}

static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

// Load libduktape on first use.
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

extern int list_size(void*);
extern void* list_get_raw(void*, int);
extern const char* aether_string_data(const void*);

#include "aether_sandbox_match.h"

static int check_sandbox(const char* category, const char* resource) {
    return aether_sandbox_check(category, resource);
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

    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;
    aether_sandbox_push(perms);

    int result = 0;
    if (dh_peval_string(ctx, code) != 0) {
        fprintf(stderr, "[js] %s\n", dh_safe_to_string(ctx, -1));
        g_duk.duk_pop(ctx);
        result = -1;
    } else {
        g_duk.duk_pop(ctx);
    }

    aether_sandbox_pop();

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

    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;
    aether_sandbox_push(perms);

    int result = 0;
    if (dh_peval_string(ctx, code) != 0) {
        fprintf(stderr, "[js] %s\n", dh_safe_to_string(ctx, -1));
        g_duk.duk_pop(ctx);
        result = -1;
    } else {
        g_duk.duk_pop(ctx);
    }

    aether_sandbox_pop();
    duktape_current_map_token = 0;

    return result;
}

// --- Tier 2: Bidirectional Embedding API (#904) ----------------------------

void* duktape_vm_new(void) {
    if (load_libduktape() != 0) return NULL;
    duk_context* c = dh_create_heap_default();
    if (!c) return NULL;
    register_bindings(c);
    return (void*)c;
}

void duktape_vm_free(void* c) {
    if (!c) return;
    duk_context* ctx_to_free = (duk_context*)c;
    if (ctx_to_free == ctx) {
        ctx = NULL;
    }
    g_duk.duk_destroy_heap(ctx_to_free);
}

static duk_ret_t duktape_host_cb_trampoline(duk_context* c) {
    g_duk.duk_push_current_function(c);
    g_duk.duk_get_prop_string(c, -1, "\xFFhost_fn");
    void* host_fn = (void*)g_duk.duk_get_pointer(c, -1);
    g_duk.duk_pop(c); // pop pointer
    g_duk.duk_pop(c); // pop function
    if (!host_fn) return 0;
    int (*fn)(void*) = (int (*)(void*))host_fn;
    return (duk_ret_t)fn((void*)c);
}

int duktape_vm_register(void* c, const char* name, void* host_fn) {
    if (!c || !name || !host_fn) return -1;
    duk_context* ctx_ptr = (duk_context*)c;

    char tmp[256];
    strncpy(tmp, name, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    g_duk.duk_push_global_object(ctx_ptr);
    int depth_pushed = 1;

    char* curr = tmp;
    char* dot = strchr(curr, '.');

    while (dot) {
        *dot = '\0';
        g_duk.duk_get_prop_string(ctx_ptr, -1, curr);
        if (g_duk.duk_get_type(ctx_ptr, -1) != DUK_TYPE_OBJECT) {
            g_duk.duk_pop(ctx_ptr);
            g_duk.duk_push_object(ctx_ptr);
            g_duk.duk_put_prop_string(ctx_ptr, -2, curr);
            g_duk.duk_get_prop_string(ctx_ptr, -1, curr);
        }
        depth_pushed++;
        curr = dot + 1;
        dot = strchr(curr, '.');
    }

    g_duk.duk_push_c_function(ctx_ptr, duktape_host_cb_trampoline, DUK_VARARGS);
    g_duk.duk_push_pointer(ctx_ptr, host_fn);
    g_duk.duk_put_prop_string(ctx_ptr, -2, "\xFFhost_fn");
    g_duk.duk_put_prop_string(ctx_ptr, -2, curr);

    for (int i = 0; i < depth_pushed; i++) {
        g_duk.duk_pop(ctx_ptr);
    }
    return 0;
}

void duktape_vm_set_global_str(void* c, const char* name, const char* value) {
    if (!c || !name) return;
    duk_context* ctx_ptr = (duk_context*)c;
    g_duk.duk_push_string(ctx_ptr, value ? value : "");
    g_duk.duk_put_global_string(ctx_ptr, name);
}

void duktape_vm_set_global_int(void* c, const char* name, int64_t value) {
    if (!c || !name) return;
    duk_context* ctx_ptr = (duk_context*)c;
    g_duk.duk_push_number(ctx_ptr, (double)value);
    g_duk.duk_put_global_string(ctx_ptr, name);
}

void duktape_vm_set_global_strlist(void* c, const char* name, void* items) {
    if (!c || !name) return;
    duk_context* ctx_ptr = (duk_context*)c;
    g_duk.duk_push_array(ctx_ptr);
    if (items) {
        int count = list_size(items);
        for (int i = 0; i < count; i++) {
            void* item = list_get_raw(items, i);
            const char* str = item ? aether_string_data(item) : "";
            g_duk.duk_push_string(ctx_ptr, str ? str : "");
            g_duk.duk_put_prop_index(ctx_ptr, -2, (duk_uarridx_t)i);
        }
    }
    g_duk.duk_put_global_string(ctx_ptr, name);
}

int duktape_vm_eval(void* c, const char* code) {
    if (!c || !code) return -1;
    duk_context* ctx_ptr = (duk_context*)c;
    duk_int_t rc = dh_peval_string(ctx_ptr, code);
    if (rc != 0) {
        return -1;
    }
    return 0;
}

int duktape_vm_result_type(void* c) {
    if (!c) return AE_DUK_TNIL;
    duk_context* ctx_ptr = (duk_context*)c;
    if (g_duk.duk_get_top(ctx_ptr) == 0) return AE_DUK_TNIL;
    if (g_duk.duk_get_error_code(ctx_ptr, -1) != 0) return AE_DUK_TERR;
    duk_int_t t = g_duk.duk_get_type(ctx_ptr, -1);
    switch (t) {
        case DUK_TYPE_UNDEFINED:
        case DUK_TYPE_NULL:
        case DUK_TYPE_NONE:
            return AE_DUK_TNIL;
        case DUK_TYPE_BOOLEAN:
            return AE_DUK_TBOOL;
        case DUK_TYPE_NUMBER:
            return AE_DUK_TINT;
        case DUK_TYPE_STRING:
            return AE_DUK_TSTR;
        case DUK_TYPE_OBJECT:
            return AE_DUK_TTABLE;
        default:
            return AE_DUK_TOTHER;
    }
}

int64_t duktape_vm_result_int(void* c) {
    if (!c) return 0;
    duk_context* ctx_ptr = (duk_context*)c;
    if (g_duk.duk_get_top(ctx_ptr) == 0) return 0;
    return (int64_t)g_duk.duk_get_number(ctx_ptr, -1);
}

const char* duktape_vm_result_str(void* c) {
    if (!c) return "";
    duk_context* ctx_ptr = (duk_context*)c;
    if (g_duk.duk_get_top(ctx_ptr) == 0) return "";
    return dh_safe_to_string(ctx_ptr, -1);
}

int duktape_vm_result_bool(void* c) {
    if (!c) return 0;
    duk_context* ctx_ptr = (duk_context*)c;
    if (g_duk.duk_get_top(ctx_ptr) == 0) return 0;
    return (int)g_duk.duk_get_boolean(ctx_ptr, -1);
}

void duktape_vm_pop_result(void* c) {
    if (!c) return;
    duk_context* ctx_ptr = (duk_context*)c;
    if (g_duk.duk_get_top(ctx_ptr) > 0) {
        g_duk.duk_pop(ctx_ptr);
    }
}

const char* duktape_vm_set_instruction_limit(void* c, int64_t budget, int every) {
    (void)c; (void)budget; (void)every;
    return "execution limit unavailable: libduktape built without DUK_USE_EXEC_TIMEOUT_CHECK";
}

int duktape_arg_count(void* c) {
    if (!c) return 0;
    return (int)g_duk.duk_get_top((duk_context*)c);
}

int duktape_arg_type(void* c, int i) {
    if (!c || i < 1) return AE_DUK_TNIL;
    duk_context* ctx_ptr = (duk_context*)c;
    duk_idx_t idx = (duk_idx_t)(i - 1);
    if (idx >= g_duk.duk_get_top(ctx_ptr)) return AE_DUK_TNIL;
    if (g_duk.duk_get_error_code(ctx_ptr, idx) != 0) return AE_DUK_TERR;
    duk_int_t t = g_duk.duk_get_type(ctx_ptr, idx);
    switch (t) {
        case DUK_TYPE_UNDEFINED:
        case DUK_TYPE_NULL:
        case DUK_TYPE_NONE:
            return AE_DUK_TNIL;
        case DUK_TYPE_BOOLEAN:
            return AE_DUK_TBOOL;
        case DUK_TYPE_NUMBER:
            return AE_DUK_TINT;
        case DUK_TYPE_STRING:
            return AE_DUK_TSTR;
        case DUK_TYPE_OBJECT:
            return AE_DUK_TTABLE;
        default:
            return AE_DUK_TOTHER;
    }
}

const char* duktape_arg_str(void* c, int i) {
    if (!c || i < 1) return "";
    duk_context* ctx_ptr = (duk_context*)c;
    duk_idx_t idx = (duk_idx_t)(i - 1);
    if (idx >= g_duk.duk_get_top(ctx_ptr)) return "";
    const char* s = g_duk.duk_get_string(ctx_ptr, idx);
    return s ? s : "";
}

int64_t duktape_arg_int(void* c, int i) {
    if (!c || i < 1) return 0;
    duk_context* ctx_ptr = (duk_context*)c;
    duk_idx_t idx = (duk_idx_t)(i - 1);
    if (idx >= g_duk.duk_get_top(ctx_ptr)) return 0;
    return (int64_t)g_duk.duk_get_number(ctx_ptr, idx);
}

int duktape_arg_bool(void* c, int i) {
    if (!c || i < 1) return 0;
    duk_context* ctx_ptr = (duk_context*)c;
    duk_idx_t idx = (duk_idx_t)(i - 1);
    if (idx >= g_duk.duk_get_top(ctx_ptr)) return 0;
    return (int)g_duk.duk_get_boolean(ctx_ptr, idx);
}

void duktape_push_int(void* c, int64_t v) {
    if (!c) return;
    g_duk.duk_push_number((duk_context*)c, (double)v);
}

void duktape_push_str(void* c, const char* v) {
    if (!c) return;
    if (v) g_duk.duk_push_string((duk_context*)c, v);
    else g_duk.duk_push_undefined((duk_context*)c);
}

void duktape_push_bool(void* c, int v) {
    if (!c) return;
    g_duk.duk_push_boolean((duk_context*)c, v != 0 ? 1 : 0);
}

void duktape_push_nil(void* c) {
    if (!c) return;
    g_duk.duk_push_null((duk_context*)c);
}

void duktape_push_double(void* c, double v) {
    if (!c) return;
    g_duk.duk_push_number((duk_context*)c, v);
}

int duktape_raise_error(void* c, const char* msg) {
    if (!c) return 0;
    duk_context* ctx_ptr = (duk_context*)c;
    g_duk.duk_push_error_object_raw(ctx_ptr, 1 /* DUK_ERR_ERROR */, NULL, 0, "%s", msg ? msg : "error");
    g_duk.duk_throw_raw(ctx_ptr);
    return 0;
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

void* duktape_vm_new(void) { duktape_init(); return NULL; }
void duktape_vm_free(void* c) { (void)c; }
int duktape_vm_register(void* c, const char* name, void* host_fn) { (void)c; (void)name; (void)host_fn; return -1; }
void duktape_vm_set_global_str(void* c, const char* name, const char* value) { (void)c; (void)name; (void)value; }
void duktape_vm_set_global_int(void* c, const char* name, int64_t value) { (void)c; (void)name; (void)value; }
void duktape_vm_set_global_strlist(void* c, const char* name, void* items) { (void)c; (void)name; (void)items; }
int duktape_vm_eval(void* c, const char* code) { (void)c; (void)code; return -1; }
int duktape_vm_result_type(void* c) { (void)c; return 0; }
int64_t duktape_vm_result_int(void* c) { (void)c; return 0; }
const char* duktape_vm_result_str(void* c) { (void)c; return ""; }
int duktape_vm_result_bool(void* c) { (void)c; return 0; }
void duktape_vm_pop_result(void* c) { (void)c; }
const char* duktape_vm_set_instruction_limit(void* c, int64_t budget, int every) { (void)c; (void)budget; (void)every; return "execution limit unavailable: libduktape built without DUK_USE_EXEC_TIMEOUT_CHECK"; }
int duktape_arg_count(void* c) { (void)c; return 0; }
int duktape_arg_type(void* c, int i) { (void)c; (void)i; return 0; }
const char* duktape_arg_str(void* c, int i) { (void)c; (void)i; return ""; }
int64_t duktape_arg_int(void* c, int i) { (void)c; (void)i; return 0; }
int duktape_arg_bool(void* c, int i) { (void)c; (void)i; return 0; }
void duktape_push_int(void* c, int64_t v) { (void)c; (void)v; }
void duktape_push_str(void* c, const char* v) { (void)c; (void)v; }
void duktape_push_bool(void* c, int v) { (void)c; (void)v; }
void duktape_push_nil(void* c) { (void)c; }
void duktape_push_double(void* c, double v) { (void)c; (void)v; }
int duktape_raise_error(void* c, const char* msg) { (void)c; (void)msg; return 0; }
#endif
