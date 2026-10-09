// contrib.host.tinygo — in-process Go via TinyGo c-shared.
// See aether_host_tinygo.h for the API contract.
#include "aether_host_tinygo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../runtime/utils/aether_compiler.h"  // AETHER_TLS
#include "../../../std/dl/aether_dl.h"
#include "../../../std/string/aether_string.h"     // the _owned twins

#ifdef AETHER_HAS_LIBFFI
#include <ffi.h>
#endif

#define AETHER_HOST_TINYGO_ERR_CAP 512

static AETHER_TLS char g_tg_last_err[AETHER_HOST_TINYGO_ERR_CAP];

static void tg_clear_error(void) { g_tg_last_err[0] = '\0'; }

static void tg_set_error(const char* fmt, const char* arg) {
    snprintf(g_tg_last_err, sizeof(g_tg_last_err), fmt, arg ? arg : "");
}

static void* tg_resolve(void* handle, const char* sym) {
    tg_clear_error();
    if (handle == NULL) {
        tg_set_error("tinygo: handle is NULL%s", "");
        return NULL;
    }
    if (sym == NULL || sym[0] == '\0') {
        tg_set_error("tinygo: symbol name is empty%s", "");
        return NULL;
    }
    void* p = aether_dl_symbol_raw(handle, sym);
    if (p == NULL) {
        snprintf(g_tg_last_err, sizeof(g_tg_last_err),
                 "tinygo: %s: %s", sym, aether_dl_last_error_raw());
        return NULL;
    }
    return p;
}

const char* aether_host_tinygo_last_error(void) {
    return g_tg_last_err;
}

// =================================================================
// Fixed-signature wrapper macros
// =================================================================
//
// Each wrapper resolves the symbol, casts to the right function-
// pointer type, invokes, returns the result. Zero overhead beyond
// the lookup + indirect call. The macros below generate every
// signature listed in aether_host_tinygo.h.
//
// DEF_R0(name, ret, default, fnty)            — 0-arg
// DEF_R1(name, ret, default, fnty, A1)         — 1-arg
// DEF_R2(name, ret, default, fnty, A1, A2)     — 2-arg
// DEF_R3(name, ret, default, fnty, A1, A2, A3) — 3-arg
// DEF_V0..V3 — same shape but for void return.

#define DEF_R0(name, ret, dflt, fnty)                                     \
    ret tinygo_call_##name(void* h, const char* s) {                      \
        void* p = tg_resolve(h, s); if (!p) return (dflt);                \
        ret (*fn)(void) = (fnty)p; return fn();                           \
    }
#define DEF_R1(name, ret, dflt, fnty, A1)                                 \
    ret tinygo_call_##name(void* h, const char* s, A1 a) {                \
        void* p = tg_resolve(h, s); if (!p) return (dflt);                \
        ret (*fn)(A1) = (fnty)p; return fn(a);                            \
    }
#define DEF_R2(name, ret, dflt, fnty, A1, A2)                             \
    ret tinygo_call_##name(void* h, const char* s, A1 a, A2 b) {          \
        void* p = tg_resolve(h, s); if (!p) return (dflt);                \
        ret (*fn)(A1, A2) = (fnty)p; return fn(a, b);                     \
    }
#define DEF_R3(name, ret, dflt, fnty, A1, A2, A3)                         \
    ret tinygo_call_##name(void* h, const char* s, A1 a, A2 b, A3 c) {    \
        void* p = tg_resolve(h, s); if (!p) return (dflt);                \
        ret (*fn)(A1, A2, A3) = (fnty)p; return fn(a, b, c);              \
    }
#define DEF_V0(name, fnty)                                                 \
    void tinygo_call_##name(void* h, const char* s) {                      \
        void* p = tg_resolve(h, s); if (!p) return;                        \
        void (*fn)(void) = (fnty)p; fn();                                  \
    }
#define DEF_V1(name, fnty, A1)                                             \
    void tinygo_call_##name(void* h, const char* s, A1 a) {                \
        void* p = tg_resolve(h, s); if (!p) return;                        \
        void (*fn)(A1) = (fnty)p; fn(a);                                   \
    }
#define DEF_V2(name, fnty, A1, A2)                                         \
    void tinygo_call_##name(void* h, const char* s, A1 a, A2 b) {          \
        void* p = tg_resolve(h, s); if (!p) return;                        \
        void (*fn)(A1, A2) = (fnty)p; fn(a, b);                            \
    }

// 0-arg
DEF_R0(i_v, int,         0,    int (*)(void))
DEF_R0(l_v, int64_t,     0,    int64_t (*)(void))
DEF_R0(d_v, double,      0.0,  double (*)(void))
DEF_R0(s_v, const char*, "",   const char* (*)(void))
DEF_R0(p_v, void*,       NULL, void* (*)(void))
DEF_V0(v_v, void (*)(void))

// 1-arg
DEF_R1(i_i, int,         0,    int (*)(int),         int)
DEF_R1(i_l, int,         0,    int (*)(int64_t),     int64_t)
DEF_R1(i_d, int,         0,    int (*)(double),      double)
DEF_R1(i_s, int,         0,    int (*)(const char*), const char*)
DEF_R1(i_p, int,         0,    int (*)(void*),       void*)
DEF_R1(l_l, int64_t,     0,    int64_t (*)(int64_t), int64_t)
DEF_R1(d_d, double,      0.0,  double (*)(double),   double)
DEF_R1(s_s, const char*, "",   const char* (*)(const char*), const char*)
DEF_R1(s_i, const char*, "",   const char* (*)(int), int)
DEF_R1(p_s, void*,       NULL, void* (*)(const char*), const char*)
DEF_R1(p_i, void*,       NULL, void* (*)(int), int)
DEF_V1(v_i, void (*)(int), int)
DEF_V1(v_s, void (*)(const char*), const char*)
DEF_V1(v_p, void (*)(void*), void*)

// 2-arg
DEF_R2(i_i_i, int,         0,    int (*)(int, int),                 int, int)
DEF_R2(i_l_l, int,         0,    int (*)(int64_t, int64_t),         int64_t, int64_t)
DEF_R2(i_s_s, int,         0,    int (*)(const char*, const char*), const char*, const char*)
DEF_R2(i_s_i, int,         0,    int (*)(const char*, int),         const char*, int)
DEF_R2(d_d_d, double,      0.0,  double (*)(double, double),        double, double)
DEF_R2(s_s_s, const char*, "",   const char* (*)(const char*, const char*),
                                                                    const char*, const char*)
DEF_V2(v_i_i, void (*)(int, int), int, int)

// 3-arg
DEF_R3(i_i_i_i, int,         0,  int (*)(int, int, int),             int, int, int)
DEF_R3(i_s_i_i, int,         0,  int (*)(const char*, int, int),     const char*, int, int)
DEF_R3(s_s_s_s, const char*, "", const char* (*)(const char*, const char*, const char*),
                                                                     const char*, const char*, const char*)

// =================================================================
// Backwards-compat aliases (round-1 shipped these names)
// =================================================================
int         tinygo_call_int_void    (void* h, const char* s)                { return tinygo_call_i_v(h, s); }
int         tinygo_call_int_int     (void* h, const char* s, int a)         { return tinygo_call_i_i(h, s, a); }
int         tinygo_call_int_int_int (void* h, const char* s, int a, int b)  { return tinygo_call_i_i_i(h, s, a, b); }
void        tinygo_call_void_int    (void* h, const char* s, int a)         { tinygo_call_v_i(h, s, a); }
const char* tinygo_call_str_str     (void* h, const char* s, const char* a) { return tinygo_call_s_s(h, s, a); }

// =================================================================
// Owned string results (#2569)
// =================================================================
//
// The `_owned` twin of each string-returning wrapper, for a Go function
// returning C.CString(...): the result is copied into an AetherString
// the caller owns and the library's pointer is freed. The failure
// default is a fresh empty string rather than the "" literal the
// borrowed wrappers return, because the caller frees what it gets.

static AetherString* tg_take(const char* r) {
    AetherString* out = string_new(r ? r : "");
    free((void*)r);
    return out;
}

#define DEF_SO0(name, fnty)                                                     \
    AetherString* tinygo_call_##name##_owned(void* h, const char* s) {          \
        void* p = tg_resolve(h, s); if (!p) return string_new("");              \
        const char* (*fn)(void) = (fnty)p; return tg_take(fn());                \
    }
#define DEF_SO1(name, fnty, A1)                                                 \
    AetherString* tinygo_call_##name##_owned(void* h, const char* s, A1 a) {    \
        void* p = tg_resolve(h, s); if (!p) return string_new("");              \
        const char* (*fn)(A1) = (fnty)p; return tg_take(fn(a));                 \
    }
#define DEF_SO2(name, fnty, A1, A2)                                             \
    AetherString* tinygo_call_##name##_owned(void* h, const char* s, A1 a,      \
                                             A2 b) {                            \
        void* p = tg_resolve(h, s); if (!p) return string_new("");              \
        const char* (*fn)(A1, A2) = (fnty)p; return tg_take(fn(a, b));          \
    }
#define DEF_SO3(name, fnty, A1, A2, A3)                                         \
    AetherString* tinygo_call_##name##_owned(void* h, const char* s, A1 a,      \
                                             A2 b, A3 c) {                      \
        void* p = tg_resolve(h, s); if (!p) return string_new("");              \
        const char* (*fn)(A1, A2, A3) = (fnty)p; return tg_take(fn(a, b, c));   \
    }

DEF_SO0(s_v,     const char* (*)(void))
DEF_SO1(s_s,     const char* (*)(const char*), const char*)
DEF_SO1(s_i,     const char* (*)(int), int)
DEF_SO2(s_s_s,   const char* (*)(const char*, const char*), const char*, const char*)
DEF_SO3(s_s_s_s, const char* (*)(const char*, const char*, const char*),
                 const char*, const char*, const char*)

AetherString* tinygo_call_str_str_owned(void* h, const char* s, const char* a) {
    return tinygo_call_s_s_owned(h, s, a);
}

// =================================================================
// libffi-backed dynamic dispatch
// =================================================================
#ifdef AETHER_HAS_LIBFFI

static ffi_type* kind_to_ffi(char k) {
    switch (k) {
        case 'v': return &ffi_type_void;
        case 'i': return &ffi_type_sint32;
        case 'l': return &ffi_type_sint64;
        case 'd': return &ffi_type_double;
        case 's': return &ffi_type_pointer;
        case 'p': return &ffi_type_pointer;
        default:  return NULL;
    }
}

int tinygo_call_dynamic(void* handle, const char* sym,
                        char return_kind, const char* arg_kinds,
                        void** args, void* result_out) {
    void* fn = tg_resolve(handle, sym);
    if (!fn) return 0;

    ffi_type* rtype = kind_to_ffi(return_kind);
    if (!rtype) {
        snprintf(g_tg_last_err, sizeof(g_tg_last_err),
                 "tinygo: bad return_kind '%c'", return_kind);
        return 0;
    }

    int nargs = 0;
    if (arg_kinds) {
        for (const char* p = arg_kinds; *p; p++) nargs++;
    }
    if (nargs > 32) {
        snprintf(g_tg_last_err, sizeof(g_tg_last_err),
                 "tinygo: too many args (max 32, got %d)", nargs);
        return 0;
    }

    ffi_type* arg_types[32];
    for (int i = 0; i < nargs; i++) {
        arg_types[i] = kind_to_ffi(arg_kinds[i]);
        if (!arg_types[i] || arg_types[i] == &ffi_type_void) {
            snprintf(g_tg_last_err, sizeof(g_tg_last_err),
                     "tinygo: bad arg_kind[%d] '%c'", i, arg_kinds[i]);
            return 0;
        }
    }

    ffi_cif cif;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)nargs, rtype, arg_types) != FFI_OK) {
        snprintf(g_tg_last_err, sizeof(g_tg_last_err),
                 "tinygo: ffi_prep_cif failed%s", "");
        return 0;
    }

    /* result_out must be at least sizeof(ffi_arg) bytes — libffi
     * widens integer returns to ffi_arg. Documented in
     * docs/middleware-design.md (caller-side). */
    ffi_call(&cif, (void (*)(void))fn, result_out, args);
    return 1;
}

#else  /* !AETHER_HAS_LIBFFI */

int tinygo_call_dynamic(void* handle, const char* sym,
                        char return_kind, const char* arg_kinds,
                        void** args, void* result_out) {
    (void)handle; (void)sym; (void)return_kind;
    (void)arg_kinds; (void)args; (void)result_out;
    snprintf(g_tg_last_err, sizeof(g_tg_last_err),
             "tinygo: libffi unavailable (rebuild with libffi for dynamic dispatch)");
    return 0;
}

#endif
