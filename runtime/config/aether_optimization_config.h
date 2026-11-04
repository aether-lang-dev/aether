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

#include <limits.h>
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
//
// __wasi__ joins __EMSCRIPTEN__ here, and it is the load-bearing arm of the
// four below. wasi has no usable threads, but zig's wasi-libc ships pthread
// STUBS (lib/libc/wasi/thread-stub/pthread_create.c) whose pthread_create
// returns EAGAIN unconditionally. So the threaded runtime does NOT fail to
// link the way aether_thread.h predicts — it links, starts, prints "Failed to
// create scheduler thread", and then spins forever on scheduler_start()'s
// readiness barrier waiting for workers that were never created. A silent
// hang is a worse failure than a missing symbol, which is why wasi must select
// the threadless path here rather than relying on a link error to catch it.
#ifndef AETHER_HAS_THREADS
#  if defined(__EMSCRIPTEN__) || defined(__wasi__) || defined(AETHER_NO_THREADING)
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

// --- Process control (fork / exec / pipe / popen / waitpid / kill) ---
//
// Separate from AETHER_HAS_FILESYSTEM because the two are genuinely different
// capabilities that happened to share a file. std/os/aether_os.c was guarded
// entirely by !AETHER_HAS_FILESYSTEM, so the only way to compile out `fork`
// was to compile out the filesystem with it. That trade is acceptable for
// Emscripten (which passes -DAETHER_NO_FILESYSTEM anyway) and wrong for WASI,
// whose whole point is a capability-based filesystem: preopened directories,
// sandboxed but real. Giving that up to dodge `fork` would disable std.fs on
// the one target that supports it properly.
//
// WASI has no process model at all — no fork, no exec, no pipe — so this is 0
// there. Emscripten is listed too: it is equally processless, and naming it
// here means the capability is honest even though its build also happens to
// pass -DAETHER_NO_FILESYSTEM today.
#ifndef AETHER_HAS_PROCESS
#  if defined(__wasi__) || defined(__EMSCRIPTEN__) || defined(AETHER_NO_PROCESS)
#    define AETHER_HAS_PROCESS 0
#  else
#    define AETHER_HAS_PROCESS 1
#  endif
#endif

// --- Networking (sockets, connect, etc.) ---
#ifndef AETHER_HAS_NETWORKING
#  if defined(__EMSCRIPTEN__) || defined(__wasi__) || defined(AETHER_NO_NETWORKING)
#    define AETHER_HAS_NETWORKING 0
#  else
#    define AETHER_HAS_NETWORKING 1
#  endif
#endif

// --- NUMA (Linux libnuma / Win32 NUMA API) ---
#ifndef AETHER_HAS_NUMA
#  if defined(AETHER_NO_NUMA) || defined(__EMSCRIPTEN__) || defined(__wasi__)
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
#  if defined(AETHER_NO_AFFINITY) || defined(__EMSCRIPTEN__) || defined(__wasi__)
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
// ENVIRONMENT VARIABLE HELPERS
// ============================================================================

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
    // Every flag bit set. An enumerator is an int before C23, so a flag is
    // at most (1 << 30) and INT_MAX holds every one of them, those added
    // later too; 0xFFFFFFFF did not fit, which -std=c11 -pedantic refuses
    // (#3548).
    AETHER_OPT_ALL              = INT_MAX
} AetherOptFlags;

// Current runtime configuration
typedef struct {
    // Tier 3 opt-in flags (user-controlled)
    atomic_bool use_lockfree_mailbox;
    atomic_bool use_message_dedup;
    atomic_bool verbose;

    // Live actors (spawned, not yet released)
    atomic_int actor_count;

    // Cooperative preemption (opt-in, zero cost when disabled)
    // Scheduler-side: break out of drain loop if handler exceeds threshold
    atomic_bool preempt_enabled;         // AETHER_PREEMPT=1 env var
    uint64_t preempt_threshold_ns;       // Default 1ms (1000000 ns)

    // Statistics
    atomic_uint_fast64_t actors_malloced;
    atomic_uint_fast64_t direct_sends;
    atomic_uint_fast64_t queue_sends;
    atomic_uint_fast64_t batches_adjusted;
    atomic_uint_fast64_t simd_batches;
    atomic_uint_fast64_t messages_deduped;
} AetherRuntimeConfig;

extern AetherRuntimeConfig g_aether_config;

// Initialize with user flags
void aether_runtime_configure(AetherOptFlags flags);

// Print current configuration
void aether_print_config(void);

// Print it once per process, however many ways ask (#3375): AETHER_VERBOSE in
// the environment when the scheduler starts, a program compiled with
// -DAETHER_VERBOSE, AETHER_OPT_VERBOSE.
void aether_print_config_once(void);

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

// Called at spawn time to count the actor
static inline void aether_on_actor_spawn(void) {
    atomic_fetch_add_explicit(&g_aether_config.actor_count, 1, memory_order_relaxed);
}

// Called when an actor terminates
static inline void aether_on_actor_terminate(void) {
    atomic_fetch_sub_explicit(&g_aether_config.actor_count, 1, memory_order_relaxed);
}

// Read the runtime settings from the environment (AETHER_VERBOSE,
// AETHER_PREEMPT, AETHER_PREEMPT_MS)
// Called once at runtime init
void aether_init_from_env(void);

#endif // AETHER_OPTIMIZATION_CONFIG_H
