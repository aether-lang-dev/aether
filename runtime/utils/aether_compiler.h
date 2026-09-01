// Aether Portable Compiler Macros
// Abstracts GCC/Clang extensions so the codebase compiles cleanly on MSVC,
// GCC, Clang, and any C11-compliant compiler without workarounds.
//
// Usage: #include "aether_compiler.h" in any file that uses these macros.

#ifndef AETHER_COMPILER_H
#define AETHER_COMPILER_H

#include <stddef.h>  // size_t

// ============================================================================
// COMPILER DETECTION
// ============================================================================

#ifndef AETHER_GCC_COMPAT
#  if defined(__GNUC__) || defined(__clang__)
#    define AETHER_GCC_COMPAT 1
#  else
#    define AETHER_GCC_COMPAT 0
#  endif
#endif

// ============================================================================
// BRANCH PREDICTION HINTS
// ============================================================================

#ifndef likely
#  if AETHER_GCC_COMPAT
#    define likely(x)   __builtin_expect(!!(x), 1)
#    define unlikely(x) __builtin_expect(!!(x), 0)
#  else
#    define likely(x)   (x)
#    define unlikely(x) (x)
#  endif
#endif

// ============================================================================
// FUNCTION ATTRIBUTES
// ============================================================================

#if AETHER_GCC_COMPAT
#  define AETHER_HOT      __attribute__((hot))
#  define AETHER_COLD     __attribute__((cold))
#  define AETHER_NOINLINE __attribute__((noinline))
#  define AETHER_INLINE   __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
#  define AETHER_HOT
#  define AETHER_COLD
#  define AETHER_NOINLINE __declspec(noinline)
#  define AETHER_INLINE   __forceinline
#else
#  define AETHER_HOT
#  define AETHER_COLD
#  define AETHER_NOINLINE
#  define AETHER_INLINE   inline
#endif

// ============================================================================
// CACHE-LINE ALIGNMENT
// ============================================================================

#if AETHER_GCC_COMPAT
#  define AETHER_ALIGNED(n) __attribute__((aligned(n)))
#elif defined(_MSC_VER)
#  define AETHER_ALIGNED(n) __declspec(align(n))
#else
#  define AETHER_ALIGNED(n)
#endif

// ============================================================================
// RESTRICT QUALIFIER
// ============================================================================

#if defined(_MSC_VER)
#  define AETHER_RESTRICT __restrict
#elif AETHER_GCC_COMPAT
#  define AETHER_RESTRICT __restrict__
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#  define AETHER_RESTRICT restrict
#else
#  define AETHER_RESTRICT
#endif

// ============================================================================
// THREAD-LOCAL STORAGE
// ============================================================================

#if defined(_MSC_VER)
#  define AETHER_TLS __declspec(thread)
#elif AETHER_GCC_COMPAT
// __thread predates _Thread_local and is supported by all GCC/Clang versions
// that support pthread. Using __thread preserves compatibility with older
// toolchains (GCC 4.x) and avoids C11 _Thread_local requirements.
#  define AETHER_TLS __thread
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define AETHER_TLS _Thread_local
#else
#  define AETHER_TLS  // Not thread-local — may cause issues in MT builds
#endif

// Variant for TLS variables in translation units that get linked
// into shared objects (`--emit=lib` .so output) AS WELL AS into the
// main `ae` executable. The default x86_64 ELF TLS model for
// `__thread` is Initial-Exec, which emits R_X86_64_TPOFF32
// relocations that the linker rejects when building a .so without
// -fPIC. Forcing Global-Dynamic produces relocations the dynamic
// linker can handle in any context (executable or shared object)
// at the cost of one GOT indirection per access — sub-nanosecond
// for variables read on the order of once per call.
//
// On Windows / MSVC __declspec(thread) doesn't take a model
// attribute; on bare-metal / pre-C11 fallbacks the TLS keyword is
// already empty, so the model is moot. Only the GCC/Clang ELF
// path needs the annotation.
#if AETHER_GCC_COMPAT && !defined(_WIN32) && !defined(__APPLE__)
#  define AETHER_TLS_SHARED __thread __attribute__((tls_model("global-dynamic")))
#else
#  define AETHER_TLS_SHARED AETHER_TLS
#endif

// ============================================================================
// AUTO-INIT / AUTO-FINI (constructor / destructor)
// On MSVC these are no-ops: caller must invoke the functions manually.
// ============================================================================

#if AETHER_GCC_COMPAT
#  define AETHER_CONSTRUCTOR __attribute__((constructor))
#  define AETHER_DESTRUCTOR  __attribute__((destructor))
#else
#  define AETHER_CONSTRUCTOR
#  define AETHER_DESTRUCTOR
#endif

// ============================================================================
// PREFETCH
// ============================================================================

#if AETHER_GCC_COMPAT
#  define AETHER_PREFETCH(ptr, rw, locality) __builtin_prefetch((ptr), (rw), (locality))
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#  include <intrin.h>
#  define AETHER_PREFETCH(ptr, rw, locality) _mm_prefetch((const char*)(ptr), _MM_HINT_T0)
#else
#  define AETHER_PREFETCH(ptr, rw, locality) ((void)(ptr))
#endif

// ============================================================================
// POPCOUNT
// ============================================================================

#if AETHER_GCC_COMPAT
#  define AETHER_POPCOUNT(x) __builtin_popcount((unsigned)(x))
#elif defined(_MSC_VER)
#  include <intrin.h>
#  define AETHER_POPCOUNT(x) __popcnt((unsigned)(x))
#else
static inline int aether_popcount_sw(unsigned x) {
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (int)(((x + (x >> 4)) & 0x0f0f0f0fu) * 0x01010101u) >> 24;
}
#  define AETHER_POPCOUNT(x) aether_popcount_sw((unsigned)(x))
#endif

// ============================================================================
// PORTABLE strndup
// Available on: Linux glibc, macOS (POSIX.1-2008), NOT MSVC.
// ============================================================================

#if defined(_WIN32) || !defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L)
#  if defined(_WIN32)  // Only define if genuinely missing (MSVC/MinGW)
#    include <stdlib.h>
#    include <string.h>
static inline char* aether_strndup_impl(const char* s, size_t n) {
    size_t len = 0;
    while (len < n && s[len]) len++;
    char* out = (char*)malloc(len + 1);
    if (out) { memcpy(out, s, len); out[len] = '\0'; }
    return out;
}
#    ifndef strndup
#      define strndup(s, n) aether_strndup_impl((s), (n))
#    endif
#    ifdef _MSC_VER
#      ifndef strdup
#        define strdup _strdup
#      endif
#    endif
#  endif
#endif

// ============================================================================
// CPU PAUSE / YIELD (for spin-wait loops)
// Reduces memory-order bus traffic and power consumption during spinning.
// ============================================================================

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#  if AETHER_GCC_COMPAT
#    define AETHER_CPU_PAUSE() __asm__ __volatile__("pause" ::: "memory")
#  elif defined(_MSC_VER)
#    include <intrin.h>
#    define AETHER_CPU_PAUSE() _mm_pause()
#  else
#    define AETHER_CPU_PAUSE() ((void)0)
#  endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#  if AETHER_GCC_COMPAT
#    define AETHER_CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#  elif defined(_MSC_VER)
#    define AETHER_CPU_PAUSE() __yield()
#  else
#    define AETHER_CPU_PAUSE() ((void)0)
#  endif
#else
#  define AETHER_CPU_PAUSE() ((void)0)
#endif

// ============================================================================
// PORTABLE fmemopen
// Available on: Linux glibc, macOS 10.13+. NOT available on Windows.
// Guard usage with AETHER_HAS_FMEMOPEN.
// ============================================================================

#if defined(_WIN32)
#  define AETHER_HAS_FMEMOPEN 0
#else
#  define AETHER_HAS_FMEMOPEN 1
#endif

#endif // AETHER_COMPILER_H
