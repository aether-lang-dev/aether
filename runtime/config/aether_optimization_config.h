// Aether Runtime Optimization Configuration
// Defines which optimizations are always-on, auto-detected, or opt-in
//
// OPTIMIZATION TIERS:
// ====================
// Tier 0 - PLATFORM CAPABILITIES (compile-time detection):
//   - AETHER_HAS_THREADS, AETHER_HAS_ATOMICS, AETHER_HAS_FILESYSTEM, etc.
//   - Auto-detected from target platform, overridable via -DAETHER_NO_*
//
// Tier 1 - ALWAYS ON (built-in, load-bearing):
//   - Actor Pooling (reuses actor structs across spawn/shutdown)
//   - Direct Send (same-core bypass)
//   - Adaptive Batching (4-64 dynamic)
//   - Message Coalescing (amortises atomics across bursts)
//   - TLS Message Pools (eliminates mutex contention on the payload allocator)
//
// Tier 2 - AUTO-DETECT (hardware-dependent):
//   - SIMD Batch Processing (requires AVX2/NEON)
//   - MWAIT Idle (requires x86 MONITOR/MWAIT)
//   - CPU Core Pinning (OS-dependent)
//
// Tier 3 - OPT-IN (user must enable via flag, trade-offs worth measuring):
//   - Lock-free Mailbox (slower single-threaded vs the locked variant;
//     the win only shows under multi-producer contention — measure on
//     your workload before enabling).
//   - Message Deduplication (adds overhead, changes semantics — filters duplicates).

#ifndef AETHER_OPTIMIZATION_CONFIG_H
#define AETHER_OPTIMIZATION_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "../utils/aether_compiler.h"

// TargetConditionals.h is what distinguishes iOS/tvOS/watchOS from macOS:
// every Apple platform defines __APPLE__, so __APPLE__ alone cannot tell a
// Mac from an iPhone. Needed by AETHER_HAS_SHELL below.
#if defined(__APPLE__)
#  include <TargetConditionals.h>
#endif

// ============================================================================
// TIER 0: PLATFORM CAPABILITIES (compile-time detection)
// Auto-detected from target platform. Override with -DAETHER_NO_<FEATURE>.
// These flags MUST be resolved before any other header is included because
// they control whether <stdatomic.h>, <pthread.h>, etc. are pulled in.
// ============================================================================

// --- Threads (pthreads / Win32 threads) ---
#ifndef AETHER_HAS_THREADS
#  if defined(__EMSCRIPTEN__) || defined(AETHER_NO_THREADING)
#    define AETHER_HAS_THREADS 0
#  else
#    define AETHER_HAS_THREADS 1
#  endif
#endif

// --- C11 Atomics ---
#ifndef AETHER_HAS_ATOMICS
#  if defined(AETHER_NO_ATOMICS) || (defined(__STDC_NO_ATOMICS__))
#    define AETHER_HAS_ATOMICS 0
#  else
#    define AETHER_HAS_ATOMICS 1
#  endif
#endif

// --- Filesystem (stdio, fopen, stat, etc.) ---
#ifndef AETHER_HAS_FILESYSTEM
#  if defined(AETHER_NO_FILESYSTEM)
#    define AETHER_HAS_FILESYSTEM 0
#  else
#    define AETHER_HAS_FILESYSTEM 1
#  endif
#endif

// --- Shell-out (system(3)) ---
// iOS/tvOS/watchOS mark system() __API_UNAVAILABLE, so referencing it is a
// hard compile error, not a link-time or runtime failure — the whole
// translation unit fails. Sandboxed Apple platforms have no shell to exec
// into, so this is a real capability axis, not a portability wart. popen()
// and the fork/exec path are NOT affected and stay available.
#ifndef AETHER_HAS_SHELL
#  if defined(AETHER_NO_SHELL)
#    define AETHER_HAS_SHELL 0
#  elif defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#    define AETHER_HAS_SHELL 0
#  else
#    define AETHER_HAS_SHELL 1
#  endif
#endif

// --- Networking (sockets, connect, etc.) ---
#ifndef AETHER_HAS_NETWORKING
#  if defined(__EMSCRIPTEN__) || defined(AETHER_NO_NETWORKING)
#    define AETHER_HAS_NETWORKING 0
#  else
#    define AETHER_HAS_NETWORKING 1
#  endif
#endif

// --- NUMA (Linux libnuma / Win32 NUMA API) ---
#ifndef AETHER_HAS_NUMA
#  if defined(AETHER_NO_NUMA) || defined(__EMSCRIPTEN__)
#    define AETHER_HAS_NUMA 0
#  elif defined(__linux__) || defined(_WIN32)
#    define AETHER_HAS_NUMA 1
#  else
#    define AETHER_HAS_NUMA 0
#  endif
#endif

// --- SIMD (AVX2/NEON/SSE) ---
#ifndef AETHER_HAS_SIMD
#  if defined(AETHER_NO_SIMD) || defined(__EMSCRIPTEN__)
#    define AETHER_HAS_SIMD 0
#  elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86) || defined(__aarch64__) || defined(__arm64__)
#    define AETHER_HAS_SIMD 1
#  else
#    define AETHER_HAS_SIMD 0
#  endif
#endif

// --- Thread Affinity (core pinning) ---
#ifndef AETHER_HAS_AFFINITY
#  if defined(AETHER_NO_AFFINITY) || defined(__EMSCRIPTEN__)
#    define AETHER_HAS_AFFINITY 0
#  elif defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
#    define AETHER_HAS_AFFINITY 1
#  else
#    define AETHER_HAS_AFFINITY 0
#  endif
#endif

// --- getenv() ---
#ifndef AETHER_HAS_GETENV
#  if defined(AETHER_NO_GETENV) || (defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 0))
#    define AETHER_HAS_GETENV 0
#  else
#    define AETHER_HAS_GETENV 1
#  endif
#endif

// --- malloc/free ---
#ifndef AETHER_HAS_MALLOC
#  if defined(AETHER_NO_MALLOC)
#    define AETHER_HAS_MALLOC 0
#  else
#    define AETHER_HAS_MALLOC 1
#  endif
#endif

// --- Atomics: real or fallback ---
#if AETHER_HAS_ATOMICS
#  include <stdatomic.h>
#else
// Single-threaded fallback: volatile serves as documentation, no real barriers needed.
// All operations are plain reads/writes — safe because only one thread exists.
// Uses inline functions instead of macros to avoid GCC statement expressions
// (which MSVC doesn't support) and to provide proper type safety.
typedef volatile int   atomic_int;
typedef volatile _Bool atomic_bool;
typedef volatile unsigned long long atomic_uint_fast64_t;
typedef struct { volatile int _Value; } atomic_flag;
#  define ATOMIC_FLAG_INIT {0}
#  define _Atomic(T) volatile T

// Memory order constants (no-ops in single-threaded)
#  define memory_order_relaxed 0
#  define memory_order_acquire 0
#  define memory_order_release 0
#  define memory_order_acq_rel 0
#  define memory_order_seq_cst 0

// Simple operations — safe as macros (no return-old-value semantics)
#  define atomic_init(p, v)                  (*(p) = (v))
#  define atomic_load(p)                     (*(p))
#  define atomic_store(p, v)                 (*(p) = (v))
#  define atomic_load_explicit(p, mo)        (*(p))
#  define atomic_store_explicit(p, v, mo)    (*(p) = (v))
#  define atomic_flag_clear(p)               ((p)->_Value = 0)
#  define atomic_flag_clear_explicit(p, mo)  ((p)->_Value = 0)
#  define atomic_thread_fence(mo)            ((void)0)

// Fetch-modify operations return the OLD value. Implemented as inline
// functions for int (the only type used in the runtime's single-threaded path).
static inline int _aether_fetch_add_int(volatile int* p, int v) { int old = *p; *p += v; return old; }
static inline int _aether_fetch_sub_int(volatile int* p, int v) { int old = *p; *p -= v; return old; }
static inline int _aether_exchange_int(volatile int* p, int v)  { int old = *p; *p = v;  return old; }
static inline bool _aether_flag_test_and_set(atomic_flag* f)    { bool old = (bool)f->_Value; f->_Value = 1; return old; }

#  define atomic_fetch_add(p, v)              _aether_fetch_add_int((volatile int*)(p), (int)(v))
#  define atomic_fetch_add_explicit(p, v, mo) _aether_fetch_add_int((volatile int*)(p), (int)(v))
#  define atomic_fetch_sub(p, v)              _aether_fetch_sub_int((volatile int*)(p), (int)(v))
#  define atomic_fetch_sub_explicit(p, v, mo) _aether_fetch_sub_int((volatile int*)(p), (int)(v))
#  define atomic_exchange(p, v)               _aether_exchange_int((volatile int*)(p), (int)(v))
#  define atomic_exchange_explicit(p, v, mo)  _aether_exchange_int((volatile int*)(p), (int)(v))
#  define atomic_flag_test_and_set(p)              _aether_flag_test_and_set(p)
#  define atomic_flag_test_and_set_explicit(p, mo) _aether_flag_test_and_set(p)

// Compare-exchange: if *p == *expected, set *p = desired and return true.
// Otherwise, set *expected = *p and return false.
static inline bool _aether_cas_int(volatile int* p, int* expected, int desired) {
    if (*p == *expected) { *p = desired; return true; }
    *expected = *p; return false;
}
#  define atomic_compare_exchange_strong(p, expected, desired) \
    _aether_cas_int((volatile int*)(p), (int*)(expected), (int)(desired))
#  define atomic_compare_exchange_strong_explicit(p, expected, desired, s, f) \
    _aether_cas_int((volatile int*)(p), (int*)(expected), (int)(desired))
#  define atomic_compare_exchange_weak(p, expected, desired) \
    _aether_cas_int((volatile int*)(p), (int*)(expected), (int)(desired))
#  define atomic_compare_exchange_weak_explicit(p, expected, desired, s, f) \
    _aether_cas_int((volatile int*)(p), (int*)(expected), (int)(desired))
#endif

// Runtime-queryable platform capabilities
typedef struct {
    bool has_threads;
    bool has_atomics;
    bool has_filesystem;
    bool has_networking;
    bool has_numa;
    bool has_simd;
    bool has_affinity;
    bool has_getenv;
    bool has_malloc;
    bool has_shell;
} AetherPlatformCaps;

static inline AetherPlatformCaps aether_platform_caps(void) {
    return (AetherPlatformCaps){
        .has_threads    = AETHER_HAS_THREADS,
        .has_atomics    = AETHER_HAS_ATOMICS,
        .has_filesystem = AETHER_HAS_FILESYSTEM,
        .has_networking = AETHER_HAS_NETWORKING,
        .has_numa       = AETHER_HAS_NUMA,
        .has_simd       = AETHER_HAS_SIMD,
        .has_affinity   = AETHER_HAS_AFFINITY,
        .has_getenv     = AETHER_HAS_GETENV,
        .has_malloc     = AETHER_HAS_MALLOC,
        .has_shell      = AETHER_HAS_SHELL,
    };
}

// ============================================================================
// MEMORY PROFILES (auto-detected or user-specified via AETHER_PROFILE)
// Controls pool sizes for different workload characteristics
// ============================================================================

typedef enum {
    AETHER_PROFILE_MICRO,    // <10 actors, <1M messages: pools=256
    AETHER_PROFILE_SMALL,    // <100 actors: pools=1024
    AETHER_PROFILE_MEDIUM,   // <1000 actors: pools=4096 (default)
    AETHER_PROFILE_LARGE     // 1000+ actors: pools=16384
} AetherProfile;

// Profile pool sizes
#define AETHER_PROFILE_MICRO_MSG_POOL   256
#define AETHER_PROFILE_SMALL_MSG_POOL   1024
#define AETHER_PROFILE_MEDIUM_MSG_POOL  4096
#define AETHER_PROFILE_LARGE_MSG_POOL   16384

// Get pool sizes for a profile
static inline int aether_profile_msg_pool_size(AetherProfile p) {
    switch (p) {
        case AETHER_PROFILE_MICRO:  return AETHER_PROFILE_MICRO_MSG_POOL;
        case AETHER_PROFILE_SMALL:  return AETHER_PROFILE_SMALL_MSG_POOL;
        case AETHER_PROFILE_LARGE:  return AETHER_PROFILE_LARGE_MSG_POOL;
        default:                    return AETHER_PROFILE_MEDIUM_MSG_POOL;
    }
}

// ============================================================================
// ENVIRONMENT VARIABLE HELPERS
// ============================================================================

// Parse AETHER_PROFILE env var
static inline AetherProfile aether_profile_from_env(void) {
#if AETHER_HAS_GETENV
    const char* env = getenv("AETHER_PROFILE");
    if (!env) return AETHER_PROFILE_MEDIUM;

    if (env[0] == 'm' && env[1] == 'i') return AETHER_PROFILE_MICRO;  // "micro"
    if (env[0] == 's') return AETHER_PROFILE_SMALL;                   // "small"
    if (env[0] == 'l') return AETHER_PROFILE_LARGE;                   // "large"
    return AETHER_PROFILE_MEDIUM;                                      // "medium" or unknown
#else
    return AETHER_PROFILE_MEDIUM;
#endif
}

// Read integer from env var with default
static inline int aether_env_int(const char* name, int default_val) {
#if AETHER_HAS_GETENV
    const char* env = getenv(name);
    if (!env) return default_val;
    return atoi(env);
#else
    (void)name;
    return default_val;
#endif
}

// Check if env var is set (any value = true, absent = false)
static inline bool aether_env_bool(const char* name) {
#if AETHER_HAS_GETENV
    return getenv(name) != NULL;
#else
    (void)name;
    return false;
#endif
}

// ============================================================================
// TIER 1: ALWAYS-ON OPTIMIZATIONS (no user control needed)
// These provide consistent wins with no downsides
// ============================================================================

#define AETHER_DIRECT_SEND_ENABLED      1   // Always bypass queue for same-core
#define AETHER_ADAPTIVE_BATCH_ENABLED   1   // Always adjust batch size dynamically
#define AETHER_COALESCING_ENABLED       1   // Always coalesce messages

// ============================================================================
// TIER 2: AUTO-DETECTED OPTIMIZATIONS (hardware/OS dependent)
// Automatically enabled if hardware supports them
// ============================================================================

// SIMD Processing - auto-detected at init
typedef struct {
    bool simd_available;        // CPU supports AVX2 or NEON
    bool mwait_available;       // CPU supports MONITOR/MWAIT
    bool cpu_pinning_available; // OS supports thread affinity
    int  simd_width;            // 128 (SSE), 256 (AVX2), or 512 (AVX-512)
    int  cache_line_size;       // For alignment (usually 64)
} AetherHardwareCapabilities;

// Global hardware caps (initialized once at runtime start)
extern AetherHardwareCapabilities g_hw_caps;

// Initialize hardware detection (called by scheduler_init)
void aether_detect_hardware(void);

// Quick checks for auto-detected features
static inline bool aether_has_simd(void) { return g_hw_caps.simd_available; }
static inline bool aether_has_mwait(void) { return g_hw_caps.mwait_available; }
static inline bool aether_has_cpu_pinning(void) { return g_hw_caps.cpu_pinning_available; }
static inline int  aether_simd_width(void) { return g_hw_caps.simd_width; }

// ============================================================================
// TIER 3: OPT-IN OPTIMIZATIONS (user-controlled via flags)
// May have trade-offs, user decides based on workload
// ============================================================================

// User-controllable optimization flags
typedef enum {
    AETHER_OPT_NONE             = 0,
    
    // Tier 3 opt-in flags (have trade-offs)
    AETHER_OPT_LOCKFREE_MAILBOX = (1 << 0),  // Slower single-threaded; faster under multi-producer contention. Measure your workload before enabling.
    AETHER_OPT_MESSAGE_DEDUP    = (1 << 1),  // Filters duplicate messages (semantic change + overhead)
    AETHER_OPT_VERBOSE          = (1 << 3),  // Print optimization info at startup
    
    // Convenience combos
    AETHER_OPT_HIGH_CONTENTION  = AETHER_OPT_LOCKFREE_MAILBOX,  // For heavy multi-threaded workloads
    AETHER_OPT_ALL              = 0xFFFFFFFF
} AetherOptFlags;

// Current runtime configuration
typedef struct {
    // Tier 3 opt-in flags (user-controlled)
    atomic_bool use_lockfree_mailbox;
    atomic_bool use_message_dedup;
    atomic_bool verbose;

    // Inline mode (auto-detected or env override)
    // When active, single-actor programs bypass queues entirely
    atomic_bool inline_mode_active;
    atomic_bool inline_mode_forced;   // Set by AETHER_INLINE=1 env var
    atomic_bool inline_mode_disabled; // Set by AETHER_NO_INLINE=1 env var

    // Main thread actor mode: single actor, all sends from main(), no scheduler needed
    // When active: messages processed synchronously in sender's context
    atomic_bool main_thread_mode;
    void* main_actor;  // Pointer to the single actor (ActorBase*)

    // Actor tracking for auto-detection
    atomic_int actor_count;

    // Memory profile (auto-detected or env override)
    AetherProfile profile;
    int msg_pool_size;    // Actual pool size (env override or from profile)

    // Cooperative preemption (opt-in, zero cost when disabled)
    // Scheduler-side: break out of drain loop if handler exceeds threshold
    atomic_bool preempt_enabled;         // AETHER_PREEMPT=1 env var
    uint64_t preempt_threshold_ns;       // Default 1ms (1000000 ns)

    // Statistics
    atomic_uint_fast64_t actors_malloced;
    atomic_uint_fast64_t direct_sends;
    atomic_uint_fast64_t queue_sends;
    atomic_uint_fast64_t inline_sends;     // Messages sent via inline mode
    atomic_uint_fast64_t batches_adjusted;
    atomic_uint_fast64_t simd_batches;
    atomic_uint_fast64_t messages_deduped;
} AetherRuntimeConfig;

extern AetherRuntimeConfig g_aether_config;

// Initialize with user flags
void aether_runtime_configure(AetherOptFlags flags);

// Print current configuration
void aether_print_config(void);

// ============================================================================
// IMPLEMENTATION (inline for performance)
// ============================================================================

static inline void aether_enable_opt(AetherOptFlags flag) {
    if (flag & AETHER_OPT_LOCKFREE_MAILBOX) atomic_store(&g_aether_config.use_lockfree_mailbox, true);
    if (flag & AETHER_OPT_MESSAGE_DEDUP) atomic_store(&g_aether_config.use_message_dedup, true);
    if (flag & AETHER_OPT_VERBOSE) atomic_store(&g_aether_config.verbose, true);
}

static inline void aether_disable_opt(AetherOptFlags flag) {
    if (flag & AETHER_OPT_LOCKFREE_MAILBOX) atomic_store(&g_aether_config.use_lockfree_mailbox, false);
    if (flag & AETHER_OPT_MESSAGE_DEDUP) atomic_store(&g_aether_config.use_message_dedup, false);
    if (flag & AETHER_OPT_VERBOSE) atomic_store(&g_aether_config.verbose, false);
}

static inline bool aether_has_opt(AetherOptFlags flag) {
    if (flag & AETHER_OPT_LOCKFREE_MAILBOX && !atomic_load(&g_aether_config.use_lockfree_mailbox)) return false;
    if (flag & AETHER_OPT_MESSAGE_DEDUP && !atomic_load(&g_aether_config.use_message_dedup)) return false;
    return true;
}

// Record statistics (enabled in debug builds)
#ifdef AETHER_STATS
#define AETHER_STAT_INC(field) atomic_fetch_add(&g_aether_config.field, 1)
#else
#define AETHER_STAT_INC(field) ((void)0)
#endif

// ============================================================================
// INLINE MODE DETECTION (for single-actor sequential optimization)
// ============================================================================

// Check if inline mode is currently active (single branch, predicted false)
static inline bool aether_inline_mode_active(void) {
    return unlikely(atomic_load_explicit(&g_aether_config.inline_mode_active, memory_order_relaxed));
}

// Check if main thread actor mode is active (synchronous processing)
// This is the fastest path: no scheduler, no queues, direct function calls.
// NOTE: No __builtin_expect here — in single-actor programs this is always true
// (hinting it as unlikely would put the fast inline path in cold instruction cache).
// The CPU branch predictor learns the correct bias quickly for both cases.
static inline bool aether_main_thread_mode_active(void) {
    return atomic_load_explicit(&g_aether_config.main_thread_mode, memory_order_relaxed);
}

// Enable main thread mode for an actor (call after spawn, before any sends)
static inline void aether_enable_main_thread_mode(void* actor) {
    atomic_store_explicit(&g_aether_config.main_thread_mode, true, memory_order_relaxed);
    g_aether_config.main_actor = actor;
}

// Called at spawn time to update actor count and inline mode state
static inline void aether_on_actor_spawn(void) {
    int count = atomic_fetch_add_explicit(&g_aether_config.actor_count, 1, memory_order_relaxed);

    // Check env overrides
    if (atomic_load_explicit(&g_aether_config.inline_mode_disabled, memory_order_relaxed)) {
        return; // User forced inline off
    }

    if (atomic_load_explicit(&g_aether_config.inline_mode_forced, memory_order_relaxed)) {
        atomic_store_explicit(&g_aether_config.inline_mode_active, true, memory_order_relaxed);
        return; // User forced inline on regardless of actor count
    }

    // Auto-detect: inline mode only for single actor (count was 0 before increment)
    bool single_actor = (count == 0);
    atomic_store_explicit(&g_aether_config.inline_mode_active, single_actor, memory_order_relaxed);

    // MAIN THREAD MODE: Disable when second actor is spawned
    // This ensures multi-actor programs use scheduler threads (not synchronous processing)
    if (!single_actor) {
        atomic_store_explicit(&g_aether_config.main_thread_mode, false, memory_order_relaxed);
        g_aether_config.main_actor = NULL;
    }
}

// Called when an actor terminates
static inline void aether_on_actor_terminate(void) {
    atomic_fetch_sub_explicit(&g_aether_config.actor_count, 1, memory_order_relaxed);
}

// Initialize profile and inline mode from env vars
// Called once at runtime init
void aether_init_from_env(void);

#endif // AETHER_OPTIMIZATION_CONFIG_H
