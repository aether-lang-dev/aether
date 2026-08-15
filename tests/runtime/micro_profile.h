/**
 * Micro-Profiling Utilities for Aether Benchmarks
 * 
 * Provides cycle-accurate timing and performance counters
 * without needing external profiling tools.
 */

#ifndef MICRO_PROFILE_H
#define MICRO_PROFILE_H

/* Everything here is `micro_profile_`-prefixed: the runtime ships its own
 * profiler with a `profile_` prefix (runtime/utils/aether_runtime_profile.h),
 * and the two collided, which is why a benchmark could not include both. */

#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#if defined(_MSC_VER)
/* MSVC needs the intrinsic pragma; MinGW GCC declares __rdtsc via
 * <intrin.h> and -Werror=unknown-pragmas rejects the pragma itself. */
#pragma intrinsic(__rdtsc)
#endif
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

/* read_nanoseconds() calls clock_gettime on every non-Windows target, and the
 * counter fallback below does too, so this is not conditional: glibc does not
 * declare it through <x86intrin.h>, and building without it only "worked"
 * because nothing compiled this header. */
#ifndef _WIN32
#include <time.h>
#endif

// ============================================================================
// High-Resolution Timing
// ============================================================================

typedef struct {
    uint64_t start_cycles;
    uint64_t end_cycles;
    uint64_t total_cycles;
    uint64_t count;
} CycleTimer;

/* A free-running counter, in whatever unit the machine offers.
 *
 * x86 has RDTSC and the unit is core cycles. AArch64 exposes the generic
 * timer instead, which ticks at a fixed frequency (24 MHz on Apple silicon),
 * so a "cycles" figure there is counter ticks and comparable only with other
 * measurements on the same machine. Anywhere else the monotonic clock stands
 * in. Including <x86intrin.h> unconditionally, as this once did, meant every
 * benchmark in this directory failed to compile on ARM. */
#if defined(_WIN32) || defined(__x86_64__) || defined(__i386__)
static inline uint64_t read_cycles(void) {
    return __rdtsc();
}
#elif defined(__aarch64__)
static inline uint64_t read_cycles(void) {
    uint64_t ticks;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
}
#else
static inline uint64_t read_cycles(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

// QueryPerformanceCounter for nanosecond precision
static inline uint64_t read_nanoseconds(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart * 1000000000ULL) / freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

// ============================================================================
// Profiling Regions
// ============================================================================

static inline void micro_profile_start(CycleTimer* timer) {
    timer->start_cycles = read_cycles();
}

static inline void micro_profile_end(CycleTimer* timer) {
    timer->end_cycles = read_cycles();
    uint64_t elapsed = timer->end_cycles - timer->start_cycles;
    timer->total_cycles += elapsed;
    timer->count++;
}

static inline void micro_profile_reset(CycleTimer* timer) {
    timer->start_cycles = 0;
    timer->end_cycles = 0;
    timer->total_cycles = 0;
    timer->count = 0;
}

static inline double micro_profile_avg_cycles(const CycleTimer* timer) {
    return timer->count > 0 ? (double)timer->total_cycles / timer->count : 0.0;
}

static inline void micro_profile_print(const char* name, const CycleTimer* timer) {
    printf("  %-30s: %12llu cycles (avg: %.2f cycles/op over %llu ops)\n",
           name, 
           (unsigned long long)timer->total_cycles,
           micro_profile_avg_cycles(timer),
           (unsigned long long)timer->count);
}

// ============================================================================
// Overhead Measurement
// ============================================================================

typedef struct {
    CycleTimer with_atomic;
    CycleTimer without_atomic;
    CycleTimer mailbox_send;
    CycleTimer mailbox_receive;
    CycleTimer message_copy;
} MicroBenchResults;

// Measure atomic operation overhead
static inline void bench_atomic_overhead(MicroBenchResults* results, int iterations) {
    volatile int counter = 0;
    _Atomic int atomic_counter = 0;
    
    // Measure non-atomic increment
    micro_profile_reset(&results->without_atomic);
    micro_profile_start(&results->without_atomic);
    for (int i = 0; i < iterations; i++) {
        counter++;
    }
    micro_profile_end(&results->without_atomic);
    (void)counter; /* volatile keeps the loop honest; MinGW GCC still
                    * flags set-but-unused under -Werror without this. */
    
    // Measure atomic increment
    micro_profile_reset(&results->with_atomic);
    micro_profile_start(&results->with_atomic);
    for (int i = 0; i < iterations; i++) {
        atomic_fetch_add_explicit(&atomic_counter, 1, memory_order_relaxed);
    }
    micro_profile_end(&results->with_atomic);
}

// Print overhead comparison
static inline void print_micro_bench_results(const MicroBenchResults* results) {
    printf("\n=== Micro-Benchmark Results ===\n");
    
    if (results->with_atomic.count > 0 && results->without_atomic.count > 0) {
        double atomic_cycles = micro_profile_avg_cycles(&results->with_atomic);
        double plain_cycles = micro_profile_avg_cycles(&results->without_atomic);
        double overhead = atomic_cycles - plain_cycles;
        
        printf("Counter Increment:\n");
        printf("  Plain int:     %.2f cycles/op\n", plain_cycles);
        printf("  Atomic int:    %.2f cycles/op\n", atomic_cycles);
        printf("  Overhead:      %.2f cycles/op (%.1fx slower)\n", 
               overhead, atomic_cycles / plain_cycles);
    }
    
    if (results->mailbox_send.count > 0) {
        micro_profile_print("Mailbox Send", &results->mailbox_send);
    }
    
    if (results->mailbox_receive.count > 0) {
        micro_profile_print("Mailbox Receive", &results->mailbox_receive);
    }
    
    if (results->message_copy.count > 0) {
        micro_profile_print("Message Copy", &results->message_copy);
    }
}

// ============================================================================
// Hot Path Markers (for manual instrumentation)
// ============================================================================

#define PROFILE_REGION_START(timer) micro_profile_start(&timer)
#define PROFILE_REGION_END(timer) micro_profile_end(&timer)

// Compile-time option to disable profiling in production
#ifdef AETHER_DISABLE_PROFILING
#undef PROFILE_REGION_START
#undef PROFILE_REGION_END
#define PROFILE_REGION_START(timer) ((void)0)
#define PROFILE_REGION_END(timer) ((void)0)
#endif

// ============================================================================
// Cache Miss Estimation (Windows only, requires admin)
// ============================================================================

#ifdef _WIN32
// Note: Requires enabling Performance Counters in Windows
// See: https://docs.microsoft.com/en-us/windows/win32/perfctrs/performance-counters-portal
static inline void print_cache_info(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    printf("\nSystem Info:\n");
    printf("  Processors: %lu\n", si.dwNumberOfProcessors);
    printf("  Page Size: %lu bytes\n", si.dwPageSize);
    printf("  Cache Line: 64 bytes (assumed)\n");
}
#endif

#endif // MICRO_PROFILE_H
