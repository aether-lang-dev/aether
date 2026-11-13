// Aether Runtime Initialization and Configuration
// Provides flags for controlling optimizations

#include "aether_runtime.h"
#include "utils/aether_cpu_detect.h"
#include "utils/aether_once.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>      // _setmode, _fileno
    #include <fcntl.h>   // _O_BINARY
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
#include <errno.h>

#include <time.h>

void aether_sleep_ms(int ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    // A signal delivered while this is waiting cuts the wait short, and the
    // old usleep loop took that as the wait being over: a program that asked
    // for a minute could return in a millisecond because something -- a
    // profiler, a job-control signal, a thread the runtime started -- happened
    // to arrive, and it had no way to tell. A server whose main task slept and
    // then exited would exit under load and look like a crash.
    //
    // nanosleep reports how much of the request is left when that happens, so
    // the wait resumes with the remainder and the full duration is honoured.
    struct timespec req, rem;
    req.tv_sec  = (time_t)(ms / 1000);
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR) break;   // nothing else here is recoverable
        req = rem;
    }
#endif
}

#ifdef _WIN32
/* The Windows services a program's generated C calls, so that C needs no
 * <windows.h> (#2673). mingw-w64's winnt.h includes <x86intrin.h>, and with
 * it every AVX-512 and AVX10 intrinsics header GCC ships: about 80,000
 * lines in each program's translation unit, most of the time a build spent
 * compiling it. */

/* UTF-8 console code pages, so a program prints Unicode correctly, and
 * stdout and stderr in binary mode, so "\n" is not written as "\r\n": a
 * program's output, redirected to a file or piped on, is byte for byte what
 * it printed. A console stream also gets virtual terminal processing, so
 * the ANSI colour escapes a program writes to a terminal (std.spec's, when
 * io.is_terminal says stdout is one, #3180) are rendered rather than shown
 * as text. A pipe or a file has no console mode and is left as it is. */
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
/* The console is the shell's, shared with every program run in it, so the
 * code pages and modes changed here are given back at exit: left as they
 * were, the shell kept UTF-8 and VT processing after the program ended. */
static UINT  g_console_out_cp, g_console_in_cp;
static DWORD g_console_vt_which[2];
static DWORD g_console_vt_mode[2];
static int   g_console_vt_count;

static void aether_console_enable_vt(DWORD which) {
    HANDLE h = GetStdHandle(which);
    DWORD mode = 0;
    if (h == NULL || h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return;
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) return;
    if (!SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return;
    g_console_vt_which[g_console_vt_count] = which;
    g_console_vt_mode[g_console_vt_count] = mode;
    g_console_vt_count++;
}

static void aether_console_restore(void) {
    for (int i = g_console_vt_count - 1; i >= 0; i--) {
        HANDLE h = GetStdHandle(g_console_vt_which[i]);
        if (h != NULL && h != INVALID_HANDLE_VALUE) SetConsoleMode(h, g_console_vt_mode[i]);
    }
    if (g_console_out_cp) SetConsoleOutputCP(g_console_out_cp);
    if (g_console_in_cp) SetConsoleCP(g_console_in_cp);
}

void aether_console_init(void) {
    static int done;   /* once: a second call would record UTF-8 as the original */
    if (done) return;
    done = 1;
    g_console_out_cp = GetConsoleOutputCP();   // 0 with no console
    g_console_in_cp = GetConsoleCP();
    SetConsoleOutputCP(65001);   // CP_UTF8
    SetConsoleCP(65001);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    aether_console_enable_vt(STD_OUTPUT_HANDLE);
    aether_console_enable_vt(STD_ERROR_HANDLE);
    atexit(aether_console_restore);
}

/* The monotonic clock behind clock_ns() and actor timeouts. The generated
 * helper this replaces converted the counter through a double, which loses
 * nanoseconds once the counter is large; aether_now_ns splits the division
 * exactly. */
int64_t aether_clock_ns(void) {
    return (int64_t)aether_now_ns();
}

/* Cooperative preemption's yield point (sched_yield in generated C). */
void aether_thread_yield(void) {
    SwitchToThread();
}
#endif

// Global runtime configuration
static AetherRuntimeInitConfig g_runtime_config = {0};
/* Filled once, by an explicit aether_runtime_init or by the first
 * aether_runtime_get_config on any thread: aether_once (#3680), so a
 * reader on another thread waits for the whole configuration. */
static aether_once_t g_runtime_once = AETHER_ONCE_INIT;

// Initialize runtime with default settings. `explicit_call`: a caller's own
// aether_runtime_init, which is told when the runtime was set up already;
// the lazy one in aether_runtime_get_config is not.
static void runtime_init_once(int num_cores, int flags, int explicit_call) {
    if (!aether_once_begin(&g_runtime_once)) {
        if (explicit_call) fprintf(stderr, "Warning: Runtime already initialized\n");
        return;
    }
    
    // Detect CPU features
    const CPUInfo* cpu = cpu_get_info();
    
    // Set defaults based on CPU capabilities
    g_runtime_config.num_cores = num_cores > 0 ? num_cores : cpu_recommend_cores();
    /* AETHER_FLAG_LOCKFREE_POOLS is accepted and ignored (#3374): there is
     * no message pool, so the flag is not kept and has_feature says 0. */
    g_runtime_config.flags = flags & ~AETHER_FLAG_LOCKFREE_POOLS;
    
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
    }
    
    // Apply explicit overrides
    g_runtime_config.use_lockfree_mailbox = 
        (flags & AETHER_FLAG_LOCKFREE_MAILBOX) || 
        (flags & AETHER_FLAG_AUTO_DETECT && cpu->sse42_supported);
    
    g_runtime_config.use_lockfree_pools = 0;   /* deprecated (#3374) */

    g_runtime_config.use_mwait = 
        (flags & AETHER_FLAG_ENABLE_MWAIT) ||
        (flags & AETHER_FLAG_AUTO_DETECT && cpu->mwait_supported);
    
    g_runtime_config.use_simd = 
        (flags & AETHER_FLAG_ENABLE_SIMD) ||
        (flags & AETHER_FLAG_AUTO_DETECT && cpu->avx2_supported);
    
    /* Done before the verbose print, which reads the configuration back. */
    aether_once_done(&g_runtime_once);
    
    // Print configuration if verbose
    if (flags & AETHER_FLAG_VERBOSE) {
        aether_runtime_print_config();
    }
}

void aether_runtime_init(int num_cores, int flags) {
    runtime_init_once(num_cores, flags, 1);
}

// Get current runtime configuration
const AetherRuntimeInitConfig* aether_runtime_get_config(void) {
    // Initialize with defaults on first access (from any thread)
    runtime_init_once(0, AETHER_FLAG_AUTO_DETECT, 0);
    return &g_runtime_config;
}

// Check if a specific feature is enabled
int aether_runtime_has_feature(int feature_flag) {
    const AetherRuntimeInitConfig* config = aether_runtime_get_config();
    return (config->flags & feature_flag) != 0;
}

// Print current runtime configuration
void aether_runtime_print_config(void) {
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
void aether_runtime_shutdown(void) {
    aether_once_reset(&g_runtime_once);
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
