// contrib.host.tinygo — in-process Go via TinyGo c-shared.
//
// Two layers:
//   1. Fixed-signature wrappers (zero overhead beyond one symbol
//      lookup + indirect call). Covers ~30 common signatures across
//      int/int64/double/ptr/string return + arg permutations.
//   2. libffi-backed dynamic dispatch when AETHER_HAS_LIBFFI is
//      defined. Pass return-kind + arg-kinds + arg pointers; the
//      shim builds a CIF, invokes ffi_call, returns the result.
//      Used for signatures the fixed wrappers don't cover.
//
// Adding a new fixed wrapper is a one-line macro instantiation in
// aether_host_tinygo.c — patches welcome.
#ifndef AETHER_HOST_TINYGO_H
#define AETHER_HOST_TINYGO_H

#include <stddef.h>
#include <stdint.h>

const char* aether_host_tinygo_last_error(void);

// ---- Fixed-signature wrappers ----
//
// Naming: tinygo_call_<ret>_<arg1>_<arg2>_..._<argN>.
//   void   v
//   int    i           (32-bit)
//   long   l           (64-bit, int64_t)
//   dbl    d           (double)
//   str    s           (const char*)
//   ptr    p           (void*)
//
// Returning a "kind" the call doesn't have produces a default
// (0 / NULL / 0.0).

// 0-arg
int         tinygo_call_i_v(void* h, const char* s);
int64_t     tinygo_call_l_v(void* h, const char* s);
double      tinygo_call_d_v(void* h, const char* s);
const char* tinygo_call_s_v(void* h, const char* s);
void*       tinygo_call_p_v(void* h, const char* s);
void        tinygo_call_v_v(void* h, const char* s);

// 1-arg
int         tinygo_call_i_i(void* h, const char* s, int a);
int         tinygo_call_i_l(void* h, const char* s, int64_t a);
int         tinygo_call_i_d(void* h, const char* s, double a);
int         tinygo_call_i_s(void* h, const char* s, const char* a);
int         tinygo_call_i_p(void* h, const char* s, void* a);
int64_t     tinygo_call_l_l(void* h, const char* s, int64_t a);
double      tinygo_call_d_d(void* h, const char* s, double a);
const char* tinygo_call_s_s(void* h, const char* s, const char* a);
const char* tinygo_call_s_i(void* h, const char* s, int a);
void*       tinygo_call_p_s(void* h, const char* s, const char* a);
void*       tinygo_call_p_i(void* h, const char* s, int a);
void        tinygo_call_v_i(void* h, const char* s, int a);
void        tinygo_call_v_s(void* h, const char* s, const char* a);
void        tinygo_call_v_p(void* h, const char* s, void* a);

// 2-arg
int         tinygo_call_i_i_i(void* h, const char* s, int a, int b);
int         tinygo_call_i_l_l(void* h, const char* s, int64_t a, int64_t b);
int         tinygo_call_i_s_s(void* h, const char* s, const char* a, const char* b);
int         tinygo_call_i_s_i(void* h, const char* s, const char* a, int b);
double      tinygo_call_d_d_d(void* h, const char* s, double a, double b);
const char* tinygo_call_s_s_s(void* h, const char* s, const char* a, const char* b);
void        tinygo_call_v_i_i(void* h, const char* s, int a, int b);

// 3-arg
int         tinygo_call_i_i_i_i(void* h, const char* s, int a, int b, int c);
int         tinygo_call_i_s_i_i(void* h, const char* s, const char* a, int b, int c);
const char* tinygo_call_s_s_s_s(void* h, const char* s, const char* a, const char* b, const char* c);

// ---- Backwards-compat aliases (shipped in #261 round 1) ----
// Old names map to the new compact ones so existing callers keep
// working without touching their code.
int         tinygo_call_int_void     (void* h, const char* s);
int         tinygo_call_int_int      (void* h, const char* s, int a);
int         tinygo_call_int_int_int  (void* h, const char* s, int a, int b);
void        tinygo_call_void_int     (void* h, const char* s, int a);
const char* tinygo_call_str_str      (void* h, const char* s, const char* a);

// ---- Owned string results (#2569) ----
//
// The string wrappers above return the library's pointer and the
// caller only borrows it. Each has an `_owned` twin for a Go function
// that returns `C.CString(...)`, which cgo allocates with malloc on the
// C heap, outside the Go collector, so nothing frees it otherwise. The
// twin copies the result into a fresh AetherString the caller owns
// (module.ae declares it `@heap`, so Aether frees it) and frees the
// library's pointer with free(). A NULL result, an unresolved symbol
// and a NULL handle all return an empty string the caller owns.
//
// Only for a result allocated by malloc in the C runtime this process
// uses: a static or long-lived buffer stays with the borrowed wrapper.
struct AetherString;
struct AetherString* tinygo_call_s_v_owned      (void* h, const char* s);
struct AetherString* tinygo_call_s_s_owned      (void* h, const char* s, const char* a);
struct AetherString* tinygo_call_s_i_owned      (void* h, const char* s, int a);
struct AetherString* tinygo_call_s_s_s_owned    (void* h, const char* s, const char* a, const char* b);
struct AetherString* tinygo_call_s_s_s_s_owned  (void* h, const char* s, const char* a, const char* b,
                                                 const char* c);
struct AetherString* tinygo_call_str_str_owned  (void* h, const char* s, const char* a);

// ---- Dynamic dispatch (libffi) ----
//
// Available when AETHER_HAS_LIBFFI is defined at build time
// (auto-detected by the Makefile via pkg-config). When the build
// does not include libffi, tinygo_call_dynamic returns 0 and
// aether_host_tinygo_last_error() reports "libffi unavailable".
//
// kinds: a NUL-terminated array of single-character codes
//   'v' void  'i' int32  'l' int64  'd' double  's' const char*  'p' void*
// return_kind is one of the same codes.
// args is an array of argument-value pointers; each pointer points
// to a value of the matching kind. The result (int64-sized buffer)
// is written into *result_out.
//
// Returns 1 on success, 0 on failure (consult last_error).
int tinygo_call_dynamic(void* handle, const char* sym,
                        char return_kind, const char* arg_kinds,
                        void** args, void* result_out);

#endif
