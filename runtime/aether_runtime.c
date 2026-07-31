// Aether Runtime Initialization and Configuration
// Provides flags for controlling optimizations

#include "aether_runtime.h"
#include "utils/aether_cpu_detect.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
#endif

// Aether built-in `sleep(ms)` lowers to this. The codegen used to inline
// Sleep()/usleep() at every call site, but that meant any user `extern
// sleep(...)` declaration emitted a C `void sleep(int);` forward decl
// that conflicted with libc's `unsigned int sleep(unsigned int)`. Routing
// the built-in through aether_sleep_ms — a stable, prefixed symbol —
// removes the collision class without changing user-visible behavior.
// See GitHub issue #233.
void aether_sleep_ms(int ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    // usleep takes microseconds; ms can hit ~2 billion before overflow,
    // so promote through unsigned long long defensively.
    unsigned long long usec = (unsigned long long)ms * 1000ull;
    // Cap at ~999 seconds per call to stay inside usleep's documented
    // limit (it's only required to support up to 1e6); split longer waits.
    while (usec > 999000000ull) {
        usleep(999000000u);
        usec -= 999000000ull;
    }
    if (usec > 0) usleep((useconds_t)usec);
#endif
}

// Global runtime configuration
static AetherRuntimeInitConfig g_runtime_config = {0};
static int g_runtime_initialized = 0;

// Initialize runtime with default settings
void aether_runtime_init(int num_cores, int flags) {
    if (g_runtime_initialized) {
        fprintf(stderr, "Warning: Runtime already initialized\n");
        return;
    }
    
    // Detect CPU features
    const CPUInfo* cpu = cpu_get_info();
    
    // Set defaults based on CPU capabilities
    g_runtime_config.num_cores = num_cores > 0 ? num_cores : cpu_recommend_cores();
    g_runtime_config.flags = flags;
    
    // Auto-enable features based on CPU if AUTO flag set
    if (flags & AETHER_FLAG_AUTO_DETECT) {
        if (cpu->avx2_supported) {
            g_runtime_config.flags |= AETHER_FLAG_ENABLE_SIMD;
        }
        if (cpu->mwait_supported) {
            g_runtime_config.flags |= AETHER_FLAG_ENABLE_MWAIT;
        }
        // Always prefer lock-free on modern CPUs
        g_runtime_config.flags |= AETHER_FLAG_LOCKFREE_MAILBOX;
        g_runtime_config.flags |= AETHER_FLAG_LOCKFREE_POOLS;
    }
    
    // Apply explicit overrides
    g_runtime_config.use_lockfree_mailbox = 
        (flags & AETHER_FLAG_LOCKFREE_MAILBOX) || 
        (flags & AETHER_FLAG_AUTO_DETECT && cpu->sse42_supported);
    
    g_runtime_config.use_lockfree_pools = 
        (flags & AETHER_FLAG_LOCKFREE_POOLS) ||
        (flags & AETHER_FLAG_AUTO_DETECT);
    
    g_runtime_config.use_mwait = 
        (flags & AETHER_FLAG_ENABLE_MWAIT) ||
        (flags & AETHER_FLAG_AUTO_DETECT && cpu->mwait_supported);
    
    g_runtime_config.use_simd = 
        (flags & AETHER_FLAG_ENABLE_SIMD) ||
        (flags & AETHER_FLAG_AUTO_DETECT && cpu->avx2_supported);
    
    g_runtime_initialized = 1;
    
    // Print configuration if verbose
    if (flags & AETHER_FLAG_VERBOSE) {
        aether_runtime_print_config();
    }
}

// Get current runtime configuration
const AetherRuntimeInitConfig* aether_runtime_get_config() {
    if (!g_runtime_initialized) {
        // Initialize with defaults on first access
        aether_runtime_init(0, AETHER_FLAG_AUTO_DETECT);
    }
    return &g_runtime_config;
}

// Check if a specific feature is enabled
int aether_runtime_has_feature(int feature_flag) {
    const AetherRuntimeInitConfig* config = aether_runtime_get_config();
    return (config->flags & feature_flag) != 0;
}

// Print current runtime configuration
void aether_runtime_print_config() {
    const AetherRuntimeInitConfig* config = aether_runtime_get_config();
    const CPUInfo* cpu = cpu_get_info();
    
    printf("\n");
    printf("========================================\n");
    printf("  Aether Runtime Configuration\n");
    printf("========================================\n\n");
    
    printf("CPU: %s\n", cpu->cpu_brand);
    printf("Cores: %d (using %d)\n", cpu->num_cores, config->num_cores);
    printf("Cache line: %d bytes\n\n", cpu->cache_line_size);
    
    printf("Active Optimizations:\n");
    printf("  Lock-free mailbox: %s\n", config->use_lockfree_mailbox ? "ENABLED" : "disabled");
    printf("  Lock-free pools:   %s\n", config->use_lockfree_pools ? "ENABLED" : "disabled");
    printf("  MWAIT idle:        %s", config->use_mwait ? "ENABLED" : "disabled");
    if (config->use_mwait && !cpu->mwait_supported) {
        printf(" (WARNING: CPU doesn't support MWAIT!)");
    }
    printf("\n");
    printf("  SIMD vectorize:    %s", config->use_simd ? "ENABLED" : "disabled");
    if (config->use_simd && !cpu->avx2_supported) {
        printf(" (WARNING: CPU doesn't support AVX2!)");
    }
    printf("\n\n");
    
    printf("Tier:\n");
    if (config->use_lockfree_mailbox && config->use_simd && config->use_mwait) {
        printf("  MAXIMUM:  lock-free mailbox + SIMD + MWAIT idle\n");
    } else if (config->use_lockfree_mailbox && config->use_simd) {
        printf("  HIGH:     lock-free mailbox + SIMD\n");
    } else if (config->use_lockfree_mailbox) {
        printf("  MODERATE: lock-free mailbox only\n");
    } else {
        printf("  BASELINE: locks-based mailbox, no SIMD, no MWAIT\n");
    }
    printf("  (Run benchmarks/cross-language to measure on this host.)\n");
    
    printf("\n========================================\n\n");
}

// Shutdown runtime
void aether_runtime_shutdown() {
    g_runtime_initialized = 0;
}

// Command-line arguments
int aether_argc = 0;
char** aether_argv = NULL;
/* Sticky one-shot seal — see aether_args_seal() below. Once set
 * aether_args_init() also refuses to (re)populate, so calling
 * aether_args_seal() from main early in startup remains correct
 * even if some embedder path later re-invokes aether_args_init(). */
static int aether_argv_sealed_flag = 0;

void aether_args_init(int argc, char** argv) {
    if (aether_argv_sealed_flag) return;
    aether_argc = argc;
    aether_argv = argv;
}

int aether_args_count(void) {
    return aether_argc;
}

const char* aether_args_get(int index) {
    if (index < 0 || index >= aether_argc || !aether_argv) {
        return NULL;
    }
    return aether_argv[index];
}

// argv[0] convenience. Exposed via std.os as `extern aether_argv0` so
// Aether programs can discover the path the OS launched them with
// without having to call `aether_args_get(0)`. Lives here (next to
// aether_argc / aether_argv) rather than in std/os/aether_os.c so that
// the compiler binary — which does NOT link aether_runtime.c — doesn't
// pull in an unresolved reference to the runtime's argv storage. The
// returned pointer is borrowed from OS argv and must not be freed.
const char* aether_argv0(void) {
    if (aether_argc <= 0 || !aether_argv) return NULL;
    return aether_argv[0];
}

char** aether_argv_raw(void) {
    return aether_argv;
}

/* One-shot argv seal — see the header comment for the threat model
 * and intended use. Clears the runtime's argv storage so subsequent
 * accessor calls fall through their existing null / out-of-range
 * paths and return 0 / NULL naturally — no extra branches needed in
 * the hot accessors. Idempotent. */
void aether_args_seal(void) {
    aether_argv_sealed_flag = 1;
    aether_argc = 0;
    aether_argv = NULL;
}

int aether_args_sealed(void) {
    return aether_argv_sealed_flag;
}
