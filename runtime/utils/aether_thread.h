// Aether Portable Thread Primitives
// Thin compatibility layer that maps the pthreads API to platform-native
// threading on all supported targets:
//
//   POSIX (Linux, macOS)  →  <pthread.h> directly (zero overhead)
//   Windows (MSVC/MinGW)  →  Win32: CRITICAL_SECTION + CONDITION_VARIABLE + FLS
//
// Usage: replace every `#include <pthread.h>` with
//        `#include "path/to/aether_thread.h"`.
//        All pthread_* call sites remain unchanged.
//
// Windows primitives chosen:
//   pthread_mutex_t  → CRITICAL_SECTION  (user-mode, no kernel transition when uncontested)
//   pthread_cond_t   → CONDITION_VARIABLE (Vista+, works natively with CRITICAL_SECTION)
//   pthread_key_t    → FLS (Fiber Local Storage) — unlike TLS, FLS supports per-key
//                       destructors called on thread exit, matching pthreads semantics
//   pthread_t        → HANDLE (from CreateThread)

#ifndef AETHER_THREAD_H
#define AETHER_THREAD_H

#include "../config/aether_optimization_config.h"

#if !AETHER_HAS_THREADS

// ============================================================
// No-thread path — single-threaded stubs for threadless platforms
// (WASM, embedded, bare-metal, or forced via -DAETHER_NO_THREADING)
// ============================================================
//
// Strategy: on hosted platforms (Linux, macOS) the system headers already
// define pthread types — we include <pthread.h> for the types and provide
// no-op inline function stubs that shadow the real functions.
// On bare-metal/freestanding, we define our own minimal types.

// Include system pthread types. Every toolchain we target provides them:
//   - Linux/macOS: <pthread.h> (full POSIX)
//   - Emscripten:  <pthread.h> (stub types when threads disabled)
//   - ARM newlib:  <sys/_pthreadtypes.h> via <sys/types.h> (from <stdio.h>)
//   - Windows:     handled by the #elif _WIN32 path below (not here)
//
// We only provide our own typedefs for truly bare-metal environments
// where no system headers define pthread types at all.
//   - WASI:        <pthread.h> IS shipped by wasi-libc, with real types, even
//                  though its pthread_create is a stub that returns EAGAIN
//                  (#1655). Include it: defining our own `typedef int
//                  pthread_mutex_t` alongside would be a redefinition error,
//                  which is exactly what happened before it was listed here.
#if defined(_POSIX_THREADS) || defined(__unix__) || defined(__linux__) || \
    defined(__APPLE__) || defined(__EMSCRIPTEN__) || defined(__wasi__) || \
    defined(_NEWLIB_VERSION)
#include <pthread.h>
#else
// Bare-metal without newlib: define minimal types
typedef int pthread_mutex_t;
typedef int pthread_cond_t;
typedef int pthread_key_t;
typedef int pthread_t;
typedef int pthread_attr_t;
typedef int pthread_mutexattr_t;
typedef int pthread_condattr_t;
/* Read-write locks: std/config/aether_config.c uses them, and a threadless
 * build failed with "unknown type name 'pthread_rwlock_t'" (#1655). These
 * belong INSIDE this bare-metal-only branch — a platform that ships
 * <pthread.h> (wasi included) already has the real types, and defining ours
 * next to them is a redefinition error. */
typedef int pthread_rwlock_t;
typedef int pthread_rwlockattr_t;
#endif



// No-op function stubs — these shadow the real pthread functions.
// On hosted platforms with -DAETHER_NO_THREADING, the linker picks our
// inline stubs over the library functions (which we never call anyway).
static inline int aether_nop_mutex_init(pthread_mutex_t* m, const void* a) { (void)m; (void)a; return 0; }
static inline int aether_nop_mutex_destroy(pthread_mutex_t* m) { (void)m; return 0; }
static inline int aether_nop_mutex_lock(pthread_mutex_t* m) { (void)m; return 0; }
static inline int aether_nop_mutex_trylock(pthread_mutex_t* m) { (void)m; return 0; }
static inline int aether_nop_mutex_unlock(pthread_mutex_t* m) { (void)m; return 0; }
static inline int aether_nop_cond_init(pthread_cond_t* c, const void* a) { (void)c; (void)a; return 0; }
static inline int aether_nop_cond_destroy(pthread_cond_t* c) { (void)c; return 0; }
static inline int aether_nop_cond_signal(pthread_cond_t* c) { (void)c; return 0; }
static inline int aether_nop_cond_broadcast(pthread_cond_t* c) { (void)c; return 0; }
static inline int aether_nop_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) { (void)c; (void)m; return 0; }
static inline int aether_nop_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const void* t) { (void)c; (void)m; (void)t; return 0; }
static inline int aether_nop_key_create(pthread_key_t* k, void (*d)(void*)) { (void)k; (void)d; return 0; }
static inline int aether_nop_key_delete(pthread_key_t k) { (void)k; return 0; }
static inline int aether_nop_setspecific(pthread_key_t k, const void* v) { (void)k; (void)v; return 0; }
static inline void* aether_nop_getspecific(pthread_key_t k) { (void)k; return NULL; }
static inline int aether_nop_rwlock_init(pthread_rwlock_t* l, const void* a) { (void)l; (void)a; return 0; }
static inline int aether_nop_rwlock_destroy(pthread_rwlock_t* l) { (void)l; return 0; }
static inline int aether_nop_rwlock_rdlock(pthread_rwlock_t* l) { (void)l; return 0; }
static inline int aether_nop_rwlock_wrlock(pthread_rwlock_t* l) { (void)l; return 0; }
static inline int aether_nop_rwlock_unlock(pthread_rwlock_t* l) { (void)l; return 0; }
static inline int aether_nop_detach(pthread_t t) { (void)t; return 0; }

/* pthread_join IS stubbed (unlike pthread_create, below), because a join is
 * only reachable after a successful create. Real call sites guard on exactly
 * that — std/http/proxy/aether_proxy_pool.c joins its health-check thread only
 * `if (pool->hc_started)`, and hc_started is set only when pthread_create
 * returned 0, which cannot happen here. So the join is dead code on a
 * threadless target, but it must still COMPILE. Leaving it undeclared broke
 * the wasm32-wasi build (#1655) on code that can never run.
 *
 * pthread_create stays unstubbed on purpose: reaching one IS a logic error,
 * and the note below explains why the linker can't be trusted to catch it. */
static inline int aether_nop_join(pthread_t t, void** r) { (void)t; if (r) *r = NULL; return 0; }

// PTHREAD_MUTEX_INITIALIZER for statically-initialised locks. The function
// stubs above cover every DYNAMIC use, but a file-scope
//   static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
// needs the macro to exist too — runtime/sandbox/aether_audit.c does exactly
// that, and without this a threadless build fails with "use of undeclared
// identifier 'PTHREAD_MUTEX_INITIALIZER'" (#1655, first hit on wasm32-wasi).
// pthread_mutex_t is an int here and every lock operation is a no-op, so any
// value is correct; 0 matches the "unlocked" reading of the typedef.
#ifndef PTHREAD_MUTEX_INITIALIZER
#  define PTHREAD_MUTEX_INITIALIZER 0
#endif
#ifndef PTHREAD_COND_INITIALIZER
#  define PTHREAD_COND_INITIALIZER 0
#endif
#ifndef PTHREAD_RWLOCK_INITIALIZER
#  define PTHREAD_RWLOCK_INITIALIZER 0
#endif

// Redirect pthread calls to no-op stubs via macros
#define pthread_mutex_init(m, a)     aether_nop_mutex_init((m), (a))
#define pthread_mutex_destroy(m)     aether_nop_mutex_destroy(m)
#define pthread_mutex_lock(m)        aether_nop_mutex_lock(m)
#define pthread_mutex_trylock(m)     aether_nop_mutex_trylock(m)
#define pthread_mutex_unlock(m)      aether_nop_mutex_unlock(m)
#define pthread_cond_init(c, a)      aether_nop_cond_init((c), (a))
#define pthread_cond_destroy(c)      aether_nop_cond_destroy(c)
#define pthread_cond_signal(c)       aether_nop_cond_signal(c)
#define pthread_cond_broadcast(c)    aether_nop_cond_broadcast(c)
#define pthread_cond_wait(c, m)      aether_nop_cond_wait((c), (m))
#define pthread_cond_timedwait(c, m, t) aether_nop_cond_timedwait((c), (m), (t))
#define pthread_key_create(k, d)     aether_nop_key_create((k), (d))
#define pthread_key_delete(k)        aether_nop_key_delete(k)
#define pthread_setspecific(k, v)    aether_nop_setspecific((k), (v))
#define pthread_getspecific(k)       aether_nop_getspecific(k)
#define pthread_detach(t)            aether_nop_detach(t)
#define pthread_rwlock_init(l, a)    aether_nop_rwlock_init((l), (a))
#define pthread_rwlock_destroy(l)    aether_nop_rwlock_destroy(l)
#define pthread_rwlock_rdlock(l)     aether_nop_rwlock_rdlock(l)
#define pthread_rwlock_wrlock(l)     aether_nop_rwlock_wrlock(l)
#define pthread_rwlock_unlock(l)     aether_nop_rwlock_unlock(l)
#define pthread_join(t, r)           aether_nop_join((t), (r))

// sched_yield stub — must come after any system header that declares it
#if !defined(__linux__) && !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
#define sched_yield() ((void)0)
#endif

// NOTE: pthread_create/pthread_join are NOT stubbed — calling them on
// a threadless platform is a logic error.
//
// Do NOT rely on that being caught at link time. It is on most targets, but
// zig's wasi-libc ships pthread STUBS (lib/libc/wasi/thread-stub/) whose
// pthread_create returns EAGAIN unconditionally, so a wasi build resolves the
// symbol, links clean, and then hangs on scheduler_start()'s readiness barrier
// waiting for workers that were never created (#1655). The defence is
// selecting the right scheduler source set for such targets, not the linker.

#elif !defined(_WIN32)

// ============================================================
// POSIX path — delegate to the real pthreads implementation
// ============================================================
#include <pthread.h>

#else // _WIN32

// ============================================================
// Windows path — implement the pthread API with Win32 primitives
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>    // malloc, free
#include <stdint.h>    // uint64_t
#include <time.h>      // struct timespec (MSVC 2015+)
#include <errno.h>

// ETIMEDOUT / ENOMEM / EBUSY may not be defined on all MSVC runtimes
#ifndef ETIMEDOUT
#  define ETIMEDOUT 138
#endif
#ifndef ENOMEM
#  define ENOMEM 12
#endif
#ifndef EBUSY
#  define EBUSY 16
#endif

// ---- Types ---------------------------------------------------------------

typedef HANDLE              pthread_t;
typedef CRITICAL_SECTION    pthread_mutex_t;
typedef CONDITION_VARIABLE  pthread_cond_t;
typedef DWORD               pthread_key_t;   // FLS index

// Attribute types — Windows has no equivalents; these are accepted but ignored
typedef int  pthread_attr_t;
typedef int  pthread_mutexattr_t;
typedef int  pthread_condattr_t;

// ---- Threads -------------------------------------------------------------

// Bridge struct: carries the pthread-style fn+arg through CreateThread
typedef struct {
    void* (*fn)(void*);
    void*  arg;
} _AetherThreadBridge;

// Trampoline: Windows calls DWORD WINAPI fn(LPVOID), pthreads calls void* fn(void*)
static DWORD WINAPI _aether_thread_trampoline(LPVOID p) {
    _AetherThreadBridge* b = (_AetherThreadBridge*)p;
    void* (*fn)(void*) = b->fn;
    void* arg          = b->arg;
    free(b);
    fn(arg);
    return 0;
}

static inline int pthread_create(pthread_t* t, pthread_attr_t* attr,
                                  void* (*fn)(void*), void* arg) {
    (void)attr;
    _AetherThreadBridge* b = (malloc)(sizeof(_AetherThreadBridge));
    if (!b) return ENOMEM;
    b->fn  = fn;
    b->arg = arg;
    *t = CreateThread(NULL, 0, _aether_thread_trampoline, b, 0, NULL);
    if (!*t) { free(b); return (int)GetLastError(); }
    return 0;
}

static inline int pthread_join(pthread_t t, void** retval) {
    (void)retval;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

static inline int pthread_detach(pthread_t t) {
    CloseHandle(t);
    return 0;
}

// ---- Mutex ---------------------------------------------------------------

static inline int pthread_mutex_init(pthread_mutex_t* m, pthread_mutexattr_t* attr) {
    (void)attr;
    InitializeCriticalSection(m);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t* m) {
    DeleteCriticalSection(m);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t* m) {
    EnterCriticalSection(m);
    return 0;
}

static inline int pthread_mutex_trylock(pthread_mutex_t* m) {
    return TryEnterCriticalSection(m) ? 0 : EBUSY;
}

static inline int pthread_mutex_unlock(pthread_mutex_t* m) {
    LeaveCriticalSection(m);
    return 0;
}

// ---- Condition Variable --------------------------------------------------

static inline int pthread_cond_init(pthread_cond_t* c, pthread_condattr_t* attr) {
    (void)attr;
    InitializeConditionVariable(c);
    return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t* c) {
    (void)c;   // CONDITION_VARIABLE needs no explicit cleanup on Windows
    return 0;
}

static inline int pthread_cond_signal(pthread_cond_t* c) {
    WakeConditionVariable(c);
    return 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t* c) {
    WakeAllConditionVariable(c);
    return 0;
}

static inline int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
    SleepConditionVariableCS(c, m, INFINITE);
    return 0;
}

// pthread_cond_timedwait: abstime is absolute CLOCK_REALTIME (seconds + nanoseconds
// since the Unix epoch 1970-01-01). Windows SleepConditionVariableCS takes a
// relative timeout in milliseconds. We convert by:
//   1. Read current wall clock via GetSystemTimeAsFileTime (100ns ticks since 1601-01-01)
//   2. Subtract the Windows-to-Unix epoch offset (116444736000000000 * 100ns)
//   3. Compute relative ms = abstime_ms - now_ms (clamped to 0)
static inline int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                                          const struct timespec* abstime) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER now_100ns;
    now_100ns.LowPart  = ft.dwLowDateTime;
    now_100ns.HighPart = ft.dwHighDateTime;
    // Convert Windows epoch (1601) → Unix epoch (1970): subtract 116444736000000000 * 100ns
    uint64_t now_ms  = now_100ns.QuadPart / 10000ULL - 11644473600000ULL;
    uint64_t abs_ms  = (uint64_t)abstime->tv_sec  * 1000ULL
                     + (uint64_t)abstime->tv_nsec  / 1000000ULL;
    DWORD timeout_ms = (abs_ms > now_ms) ? (DWORD)(abs_ms - now_ms) : 0;
    BOOL ok = SleepConditionVariableCS(c, m, timeout_ms);
    return ok ? 0 : ETIMEDOUT;
}

// ---- Thread-Local Storage (via Fiber Local Storage) ----------------------
// FLS is used instead of TLS because FlsAlloc() accepts a per-key destructor
// called on thread exit — exactly matching pthread_key_create() semantics.
// TlsAlloc() has no destructor support.

static inline int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
    *key = FlsAlloc((PFLS_CALLBACK_FUNCTION)destructor);
    return (*key == FLS_OUT_OF_INDEXES) ? ENOMEM : 0;
}

static inline int pthread_key_delete(pthread_key_t key) {
    return FlsFree(key) ? 0 : (int)GetLastError();
}

static inline int pthread_setspecific(pthread_key_t key, const void* value) {
    return FlsSetValue(key, (PVOID)(uintptr_t)value) ? 0 : (int)GetLastError();
}

static inline void* pthread_getspecific(pthread_key_t key) {
    return FlsGetValue(key);
}

// ---- clock_gettime / nanosleep compat ------------------------------------
// Always provide our own clock_gettime on Windows. MinGW declares it in
// <time.h> but some toolchain versions route it through __clock_gettime64
// which is absent from the import library, causing linker failures.
// Using a renamed static inline + macro avoids both the linker issue and
// any redeclaration conflicts with MinGW's <time.h>.

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  0
#endif

static inline int aether_win32_clock_gettime(int clk_id, struct timespec* ts) {
    if (clk_id == CLOCK_MONOTONIC) {
        LARGE_INTEGER freq, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        ts->tv_sec  = (time_t)(now.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)((now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
    } else {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER uli;
        uli.LowPart  = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        uint64_t unix_100ns = uli.QuadPart - 116444736000000000ULL;
        ts->tv_sec  = (time_t)(unix_100ns / 10000000ULL);
        ts->tv_nsec = (long)((unix_100ns % 10000000ULL) * 100);
    }
    return 0;
}
#define clock_gettime(clk, ts) aether_win32_clock_gettime((clk), (ts))

// ---- nanosleep compat ----------------------------------------------------
// Same rename+macro pattern as clock_gettime to avoid MinGW redefinition.
static inline int aether_win32_nanosleep(const struct timespec* req, struct timespec* rem) {
    (void)rem;
    DWORD ms = (DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
    if (ms == 0 && req->tv_nsec > 0) ms = 1;
    Sleep(ms);
    return 0;
}
#define nanosleep(req, rem) aether_win32_nanosleep((req), (rem))

// ---- sched_yield compat --------------------------------------------------
static inline int aether_win32_sched_yield(void) {
    SwitchToThread();
    return 0;
}
#define sched_yield() aether_win32_sched_yield()

#endif // _WIN32

/* Bare-metal newlib declares clock_gettime but not the clock ids, and
 * the CLOCK_MONOTONIC fallback above lives in the Windows branch. */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

// ---- monotonic nanosecond clock -----------------------------------------
// <time.h> is included HERE rather than per-branch because aether_now_ns()
// below is shared by all three (threadless / POSIX / Windows). The POSIX
// branch got struct timespec and clock_gettime transitively from <pthread.h>
// and the Windows branch includes <time.h> itself, so only the THREADLESS
// branch was left without a declaration — which no target exercised until
// wasm32-wasi (#1655), where it surfaced as
//   error: call to undeclared function 'clock_gettime'
// Including it unconditionally is harmless for the other two.
#include <time.h>

// One implementation for every caller. On Windows this routes through the
// aether_win32_clock_gettime shim above, which splits the
// QueryPerformanceCounter division into whole seconds plus remainder.
// Open-coded copies that computed `count * 1000000000 / freq` overflow
// int64 once the counter passes ~9.2e9 ticks (about 15 minutes of uptime
// at a typical 10 MHz QPC frequency); copies going through `double` lose
// precision at nanosecond magnitudes. The epoch is arbitrary, only
// differences are meaningful.
static inline uint64_t aether_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#endif // AETHER_THREAD_H
