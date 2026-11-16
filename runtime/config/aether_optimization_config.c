// Aether Runtime Optimization Configuration - Implementation
// See aether_optimization_config.h for documentation

#include "aether_optimization_config.h"
#include "../utils/aether_cpu_detect.h"
#include "../utils/aether_once.h"
#include <stdio.h>
#include <string.h>

// Global hardware capabilities (auto-detected once)
AetherHardwareCapabilities g_hw_caps = {0};

// Global runtime config with defaults
AetherRuntimeConfig g_aether_config = {
    .use_lockfree_mailbox = false,  // Opt-in (slower single-thread)
    .use_message_dedup = false,     // Opt-in (changes semantics)
    .verbose = false,
    .actor_count = 0,
    .preempt_enabled = false,       // Opt-in: AETHER_PREEMPT=1
    .preempt_threshold_ns = 1000000, // 1ms default
    .actors_malloced = 0,
    .direct_sends = 0,
    .queue_sends = 0,
    .batches_adjusted = 0,
    .simd_batches = 0,
    .messages_deduped = 0
};

/* Detected once, by whichever thread starts a scheduler or configures the
 * runtime first (scheduler_init can run on demand on any thread):
 * aether_once (#3680), so another caller waits for g_hw_caps to be filled.
 */
static aether_once_t g_hw_once = AETHER_ONCE_INIT;

// Detect hardware capabilities (called once at startup)
void aether_detect_hardware(void) {
    if (!aether_once_begin(&g_hw_once)) return;

#if AETHER_HAS_SIMD
    const CPUInfo* cpu = cpu_get_info();

    // SIMD detection
    if (cpu->avx2_supported) {
        g_hw_caps.simd_available = true;
        g_hw_caps.simd_width = 256;  // AVX2 = 256 bits
    } else if (cpu->sse42_supported) {
        g_hw_caps.simd_available = true;
        g_hw_caps.simd_width = 128;  // SSE4.2 = 128 bits
    }
#if defined(__aarch64__)
    // ARM NEON is always available on 64-bit ARM
    g_hw_caps.simd_available = true;
    g_hw_caps.simd_width = 128;  // NEON = 128 bits
#endif

    // AVX-512 check (rare but fast)
    if (cpu->avx512f_supported) {
        g_hw_caps.simd_width = 512;
    }

    // MWAIT detection (power-efficient idle)
    g_hw_caps.mwait_available = cpu->mwait_supported;

    // Cache line size
    g_hw_caps.cache_line_size = cpu->cache_line_size > 0 ? cpu->cache_line_size : 64;
#else
    // No SIMD detection on this platform
    g_hw_caps.simd_available = false;
    g_hw_caps.simd_width = 0;
    g_hw_caps.mwait_available = false;
    g_hw_caps.cache_line_size = 64;
#endif

    // CPU pinning detection (OS-dependent)
#if AETHER_HAS_AFFINITY
#  if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    g_hw_caps.cpu_pinning_available = true;
#  else
    g_hw_caps.cpu_pinning_available = false;
#  endif
#else
    g_hw_caps.cpu_pinning_available = false;
#endif
    aether_once_done(&g_hw_once);
}

// Configure runtime with user flags
void aether_runtime_configure(AetherOptFlags flags) {
    // Always detect hardware first
    aether_detect_hardware();
    
    // Apply user's opt-in flags
    atomic_store(&g_aether_config.use_lockfree_mailbox, (flags & AETHER_OPT_LOCKFREE_MAILBOX) != 0);
    atomic_store(&g_aether_config.use_message_dedup, (flags & AETHER_OPT_MESSAGE_DEDUP) != 0);
    atomic_store(&g_aether_config.verbose, (flags & AETHER_OPT_VERBOSE) != 0);
    
    if (flags & AETHER_OPT_VERBOSE) aether_print_config_once();
}

// Initialize runtime config from environment variables
// Called once at scheduler init before any actors spawn
void aether_init_from_env(void) {
#if AETHER_HAS_GETENV
    // AETHER_VERBOSE: the configuration, printed when the scheduler starts
    // (#3375), whatever the program was compiled with. It used to set a
    // flag nothing read: only a program compiled with -DAETHER_VERBOSE
    // printed it.
    if (getenv("AETHER_VERBOSE")) {
        atomic_store(&g_aether_config.verbose, true);
        aether_print_config_once();
    }

    // Cooperative preemption (opt-in)
    if (getenv("AETHER_PREEMPT")) {
        atomic_store(&g_aether_config.preempt_enabled, true);
        int threshold = aether_env_int("AETHER_PREEMPT_MS", 0);
        if (threshold > 0) {
            g_aether_config.preempt_threshold_ns = (uint64_t)threshold * 1000000ULL;
        }
    }
#endif
}

// Once per process: the environment, the program and the flags may all ask.
static atomic_int g_config_printed = 0;

void aether_print_config_once(void) {
    if (atomic_exchange(&g_config_printed, 1) == 0) aether_print_config();
}

// Print current configuration
void aether_print_config(void) {
    printf("\n=== Aether Runtime Configuration ===\n\n");

    printf("TIER 0 - PLATFORM CAPABILITIES:\n");
    printf("  Threads:    %s\n", AETHER_HAS_THREADS    ? "YES" : "NO");
    printf("  Atomics:    %s\n", AETHER_HAS_ATOMICS    ? "YES" : "NO");
    printf("  Filesystem: %s\n", AETHER_HAS_FILESYSTEM ? "YES" : "NO");
    printf("  Networking: %s\n", AETHER_HAS_NETWORKING ? "YES" : "NO");
    printf("  NUMA:       %s\n", AETHER_HAS_NUMA       ? "YES" : "NO");
    printf("  SIMD:       %s\n", AETHER_HAS_SIMD       ? "YES" : "NO");
    printf("  Affinity:   %s\n", AETHER_HAS_AFFINITY   ? "YES" : "NO");
    printf("  getenv:     %s\n", AETHER_HAS_GETENV     ? "YES" : "NO");
    printf("  malloc:     %s\n", AETHER_HAS_MALLOC     ? "YES" : "NO");
    printf("  shell:      %s\n\n", AETHER_HAS_SHELL    ? "YES" : "NO");

    printf("TIER 1 - ALWAYS ON:\n");
    printf("  [ON] Direct Send          - Same-core bypass\n");
    printf("  [ON] Adaptive Batching    - Dynamic 4-64 batch size\n");
    printf("  [ON] Message Coalescing   - Amortises atomics across bursts\n\n");

    printf("TIER 2 - AUTO-DETECTED:\n");
    printf("  [%s] SIMD Processing      - %d-bit vectors%s\n",
           g_hw_caps.simd_available ? "ON" : "OFF",
           g_hw_caps.simd_width,
           g_hw_caps.simd_available ? "" : " (CPU lacks AVX2/NEON)");
    printf("  [%s] MWAIT Idle           - Sub-µs wake latency%s\n",
           g_hw_caps.mwait_available ? "ON" : "OFF",
           g_hw_caps.mwait_available ? "" : " (CPU lacks MONITOR/MWAIT)");
    printf("  [%s] CPU Pinning          - Thread affinity%s\n\n",
           g_hw_caps.cpu_pinning_available ? "ON" : "OFF",
           g_hw_caps.cpu_pinning_available ? "" : " (OS unsupported)");

    printf("TIER 3 - OPT-IN:\n");
    printf("  [%s] Lock-free Mailbox   - AETHER_OPT_LOCKFREE_MAILBOX\n",
           atomic_load(&g_aether_config.use_lockfree_mailbox) ? "ON" : "off");
    printf("  [%s] Message Dedup       - AETHER_OPT_MESSAGE_DEDUP\n\n",
           atomic_load(&g_aether_config.use_message_dedup) ? "ON" : "off");

    printf("Cache line: %d bytes\n", g_hw_caps.cache_line_size);
    printf("====================================\n\n");
}
