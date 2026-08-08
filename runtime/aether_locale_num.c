// Aether — locale-independent float text conversion (see aether_locale_num.h)
//
// Three backends, chosen per-function because platform support is uneven:
//
//   1. _l-suffixed libc calls — the locale is an explicit argument, so no
//      thread state is touched at all. Best where available.
//        macOS/BSD: snprintf_l(buf, n, LOC, fmt, ...)   locale BEFORE fmt
//        Windows:   _snprintf_l(buf, n, fmt, LOC, ...)  locale AFTER fmt
//      (The argument orders genuinely differ — hence two explicit wrappers
//      rather than one aliased macro.)
//
//   2. uselocale() bracket — glibc has strtod_l/strtof_l but no snprintf_l,
//      so its emit path swaps the thread's locale for the duration of the
//      call and restores it on every return path. Thread-local, so unlike
//      setlocale it never disturbs other threads or the embedding host.
//
//   3. Plain passthrough — for platforms with no locale machinery at all
//      (WASM/emcc, ARM newlib/-ffreestanding). This is NOT the post-hoc
//      "reformat afterwards" hack this file was written to delete: on those
//      targets LC_NUMERIC can never be anything but "C", so the plain call
//      is correct by construction. Same shape as AETHER_HAS_THREADS /
//      AETHER_HAS_FILESYSTEM degrading in aether_optimization_config.h.

#define _GNU_SOURCE  // glibc: strtod_l/strtof_l, uselocale, newlocale

#include "aether_locale_num.h"
#include "config/aether_optimization_config.h"  // AETHER_HAS_THREADS / _ATOMICS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <locale.h>

// ---------------------------------------------------------------------------
// Capability detection
// ---------------------------------------------------------------------------

#ifndef AETHER_HAS_LOCALE_CONV
#  if defined(__EMSCRIPTEN__) || defined(AETHER_NO_FILESYSTEM) || \
      defined(__NEWLIB__) || defined(_NEWLIB_VERSION)
     // Freestanding / WASM: no xlocale machinery, and LC_NUMERIC is pinned to
     // "C" anyway, so there is nothing to defend against.
#    define AETHER_HAS_LOCALE_CONV 0
#  else
#    define AETHER_HAS_LOCALE_CONV 1
#  endif
#endif

#if AETHER_HAS_LOCALE_CONV && defined(_WIN32)
#  define AETHER_LOCALE_WIN32 1
#else
#  define AETHER_LOCALE_WIN32 0
#endif

// BSD/macOS expose snprintf_l and friends via <xlocale.h>.
#if AETHER_HAS_LOCALE_CONV && !AETHER_LOCALE_WIN32
#  if defined(__APPLE__) || defined(__FreeBSD__) || defined(__DragonFly__) || \
      defined(__NetBSD__) || defined(__OpenBSD__)
#    include <xlocale.h>
#    define AETHER_HAS_SNPRINTF_L 1
#  else
#    define AETHER_HAS_SNPRINTF_L 0
#  endif
#else
#  define AETHER_HAS_SNPRINTF_L 0
#endif

// ---------------------------------------------------------------------------
// The cached "C" locale handle
// ---------------------------------------------------------------------------
// Created once per process, never per call: string.from_double sits on hot
// paths (JSON serialisation, string interpolation) and newlocale is a heap
// allocation plus a category walk.
//
// Deliberately never freed. It is a single process-lifetime handle, the exact
// shape leak checkers treat as still-reachable rather than lost, and an atexit
// hook would introduce a destruction-order hazard against threads still
// formatting during shutdown.

#if AETHER_HAS_LOCALE_CONV

#if AETHER_LOCALE_WIN32

typedef _locale_t aether_loc_t;
#define AETHER_LOC_NULL ((_locale_t)0)

static aether_loc_t aether_make_c_locale(void) {
    return _create_locale(LC_ALL, "C");
}

#else  // POSIX

typedef locale_t aether_loc_t;
#define AETHER_LOC_NULL ((locale_t)0)

static aether_loc_t aether_make_c_locale(void) {
    return newlocale(LC_ALL_MASK, "C", (locale_t)0);
}

#endif

// Lazy init WITHOUT pthread_once: aether_thread.h maps pthreads onto Win32 but
// deliberately does not provide pthread_once/PTHREAD_ONCE_INIT, so relying on
// it here fails to compile on MinGW (caught on winbaz, not in review).
//
// A benign race is acceptable here and cheaper than a mutex on this hot path:
// if two threads arrive together both may build a "C" locale and one handle is
// dropped. That is at most one small allocation lost once per process; it never
// corrupts state, because each handle is independently valid and immutable, and
// whatever is published is always a usable locale. Acquire/release ordering
// makes the publication well-defined rather than a data race on a plain
// pointer.

#if AETHER_HAS_ATOMICS && AETHER_HAS_THREADS
#include <stdatomic.h>
static _Atomic(aether_loc_t) g_c_locale = AETHER_LOC_NULL;

static aether_loc_t aether_c_locale(void) {
    aether_loc_t loc = atomic_load_explicit(&g_c_locale, memory_order_acquire);
    if (!loc) {
        loc = aether_make_c_locale();
        if (loc) atomic_store_explicit(&g_c_locale, loc, memory_order_release);
    }
    return loc;
}

#else  // no atomics, or a single-threaded build: a plain guard is sufficient

static aether_loc_t g_c_locale = AETHER_LOC_NULL;

static aether_loc_t aether_c_locale(void) {
    if (!g_c_locale) g_c_locale = aether_make_c_locale();
    return g_c_locale;
}

#endif

#endif  // AETHER_HAS_LOCALE_CONV

// ---------------------------------------------------------------------------
// Emit
// ---------------------------------------------------------------------------

int aether_c_snprintf_double(char* buf, size_t n, const char* fmt, double value) {
    if (!buf || n == 0 || !fmt) return -1;

#if !AETHER_HAS_LOCALE_CONV
    // Backend 3 — LC_NUMERIC is structurally "C" on this target.
    return snprintf(buf, n, fmt, value);

#elif AETHER_LOCALE_WIN32
    // Backend 1 (Windows). _snprintf_l takes the locale AFTER the format, and
    // is NOT C99 on truncation: it returns negative and leaves the buffer
    // unterminated where C99 returns the would-be length and terminates. Both
    // are normalised here so callers can use one set of checks everywhere.
    aether_loc_t loc = aether_c_locale();
    int written = loc ? _snprintf_l(buf, n, fmt, loc, value)
                      : snprintf(buf, n, fmt, value);
    if (written < 0 || (size_t)written >= n) {
        buf[n - 1] = '\0';
        // Recover the C99 "would-be length" so the caller's
        // `written >= sizeof(buf)` truncation test still fires.
        int needed = loc ? _scprintf_l(fmt, loc, value) : _scprintf(fmt, value);
        return needed;
    }
    return written;

#elif AETHER_HAS_SNPRINTF_L
    // Backend 1 (BSD/macOS). Locale BEFORE the format here.
    {
        aether_loc_t loc = aether_c_locale();
        if (!loc) return snprintf(buf, n, fmt, value);
        return snprintf_l(buf, n, loc, fmt, value);
    }

#else
    // Backend 2 (glibc/musl). No snprintf_l, so swap the THREAD's locale for
    // the duration of the call. uselocale is thread-local: other threads and
    // the embedding host are unaffected.
    {
        aether_loc_t loc = aether_c_locale();
        if (!loc) return snprintf(buf, n, fmt, value);

        int saved_errno = errno;
        locale_t previous = uselocale(loc);
        int written = snprintf(buf, n, fmt, value);
        // ALWAYS restore, on every path. `previous` is LC_GLOBAL_LOCALE (not
        // NULL) for a thread that never called uselocale itself — the common
        // case — so a `!= NULL` guard here would be wrong in intent and would
        // strand that thread in the C locale for the rest of its life.
        // uselocale(LC_GLOBAL_LOCALE) correctly puts it back on the global one.
        uselocale(previous);
        errno = saved_errno;  // uselocale must not perturb the caller's errno
        return written;
    }
#endif
}

// ---------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------
// errno is deliberately NOT cleared here — callers set errno = 0 before the
// call and test for ERANGE after, exactly as they would around bare strtod.

double aether_c_strtod(const char* s, char** endptr) {
    if (!s) {
        if (endptr) *endptr = NULL;
        return 0.0;
    }

#if !AETHER_HAS_LOCALE_CONV
    return strtod(s, endptr);

#elif AETHER_LOCALE_WIN32
    {
        aether_loc_t loc = aether_c_locale();
        return loc ? _strtod_l(s, endptr, loc) : strtod(s, endptr);
    }

#else
    // glibc, musl and the BSDs all provide strtod_l — no uselocale needed on
    // the parse side anywhere.
    {
        aether_loc_t loc = aether_c_locale();
        return loc ? strtod_l(s, endptr, loc) : strtod(s, endptr);
    }
#endif
}

float aether_c_strtof(const char* s, char** endptr) {
    if (!s) {
        if (endptr) *endptr = NULL;
        return 0.0f;
    }

#if !AETHER_HAS_LOCALE_CONV
    return strtof(s, endptr);

#elif AETHER_LOCALE_WIN32
    {
        aether_loc_t loc = aether_c_locale();
        return loc ? _strtof_l(s, endptr, loc) : strtof(s, endptr);
    }

#else
    {
        aether_loc_t loc = aether_c_locale();
        return loc ? strtof_l(s, endptr, loc) : strtof(s, endptr);
    }
#endif
}

// ---------------------------------------------------------------------------
// Test-only locale switch
// ---------------------------------------------------------------------------

int aether_test_setlocale(const char* name) {
    if (!name) return 0;
    // LC_ALL's numeric value is libc-specific (6 on glibc, 0 elsewhere), which
    // is exactly why this lives in C rather than being an `extern setlocale`
    // with a hardcoded constant on the Aether side.
    return setlocale(LC_ALL, name) != NULL ? 1 : 0;
}
