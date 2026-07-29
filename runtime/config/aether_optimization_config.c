// Aether Runtime Optimization Configuration - Implementation
// See aether_optimization_config.h for documentation

#include "aether_optimization_config.h"
#include "../utils/aether_cpu_detect.h"
#include <stdio.h>
#include <string.h>

// Global hardware capabilities (auto-detected once)
AetherHardwareCapabilities g_hw_caps = {0};

// Global runtime config with defaults
AetherRuntimeConfig g_aether_config = {
    .use_lockfree_mailbox = false,  // Opt-in (slower single-thread)
    .use_message_dedup = false,     // Opt-in (changes semantics)
    .verbose = false,
    .inline_mode_active = false,
    .inline_mode_forced = false,
    .inline_mode_disabled = false,
    .main_thread_mode = false,      // Sync processing on main thread
    .main_actor = NULL,
    .actor_count = 0,
    .preempt_enabled = false,       // Opt-in: AETHER_PREEMPT=1
    .preempt_threshold_ns = 1000000, // 1ms default
    .profile = AETHER_PROFILE_MEDIUM,
    .msg_pool_size = AETHER_PROFILE_MEDIUM_MSG_POOL,
    .actors_malloced = 0,
    .direct_sends = 0,
    .queue_sends = 0,
    .inline_sends = 0,
    .batches_adjusted = 0,
    .simd_batches = 0,
    .messages_deduped = 0
};

static int g_hw_detected = 0;

// Detect hardware capabilities (called once at startup)
void aether_detect_hardware(void) {
    if (g_hw_detected) return;
    g_hw_detected = 1;

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
}

// Configure runtime with user flags
void aether_runtime_configure(AetherOptFlags flags) {
    // Always detect hardware first
    aether_detect_hardware();
    
    // Apply user's opt-in flags
    atomic_store(&g_aether_config.use_lockfree_mailbox, (flags & AETHER_OPT_LOCKFREE_MAILBOX) != 0);
    atomic_store(&g_aether_config.use_message_dedup, (flags & AETHER_OPT_MESSAGE_DEDUP) != 0);
    atomic_store(&g_aether_config.verbose, (flags & AETHER_OPT_VERBOSE) != 0);
    
    // Print config if verbose
    if (flags & AETHER_OPT_VERBOSE) {
        aether_print_config();
    }
}

// Initialize runtime config from environment variables
// Called once at scheduler init before any actors spawn
void aether_init_from_env(void) {
#if AETHER_HAS_GETENV
    // Check inline mode env overrides first
    if (getenv("AETHER_INLINE")) {
        atomic_store(&g_aether_config.inline_mode_forced, true);
        atomic_store(&g_aether_config.inline_mode_active, true);
    }
    if (getenv("AETHER_NO_INLINE")) {
        atomic_store(&g_aether_config.inline_mode_disabled, true);
        atomic_store(&g_aether_config.inline_mode_active, false);
    }

    // Profile from env or default
    g_aether_config.profile = aether_profile_from_env();

    // Pool size: env override > profile default
    int env_msg = aether_env_int("AETHER_MSG_POOL_SIZE", 0);

    g_aether_config.msg_pool_size = env_msg > 0
        ? env_msg
        : aether_profile_msg_pool_size(g_aether_config.profile);

    // Check verbose mode
    if (getenv("AETHER_VERBOSE")) {
        atomic_store(&g_aether_config.verbose, true);
    }

    // Cooperative preemption (opt-in)
    if (getenv("AETHER_PREEMPT")) {
        atomic_store(&g_aether_config.preempt_enabled, true);
        int threshold = aether_env_int("AETHER_PREEMPT_MS", 0);
        if (threshold > 0) {
            g_aether_config.preempt_threshold_ns = (uint64_t)threshold * 1000000ULL;
        }
    }
#else
    // No getenv: use defaults
    g_aether_config.profile = AETHER_PROFILE_MEDIUM;
    g_aether_config.msg_pool_size = aether_profile_msg_pool_size(AETHER_PROFILE_MEDIUM);
#endif
}

// Profile name helper
static const char* aether_profile_name(AetherProfile p) {
    switch (p) {
        case AETHER_PROFILE_MICRO:  return "micro";
        case AETHER_PROFILE_SMALL:  return "small";
        case AETHER_PROFILE_LARGE:  return "large";
        default:                    return "medium";
    }
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
    printf("  malloc:     %s\n\n", AETHER_HAS_MALLOC   ? "YES" : "NO");

    printf("PROFILE: %s (msg_pool=%d)\n",
           aether_profile_name(g_aether_config.profile),
           g_aether_config.msg_pool_size);

    const char* inline_status = "auto";
    if (atomic_load(&g_aether_config.inline_mode_forced)) inline_status = "forced ON (AETHER_INLINE)";
    else if (atomic_load(&g_aether_config.inline_mode_disabled)) inline_status = "forced OFF (AETHER_NO_INLINE)";
    printf("INLINE MODE: %s (current: %s)\n\n",
           inline_status,
           atomic_load(&g_aether_config.inline_mode_active) ? "active" : "inactive");

    printf("TIER 1 - ALWAYS ON:\n");
    printf("  [ON] Direct Send          - Same-core bypass\n");
    printf("  [ON] Adaptive Batching    - Dynamic 4-64 batch size\n");
    printf("  [ON] Message Coalescing   - Amortises atomics across bursts\n");
    printf("  [ON] TLS Message Pools    - Eliminates mutex contention\n\n");

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
