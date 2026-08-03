#ifndef AETHER_RUNTIME_H
#define AETHER_RUNTIME_H

#include "utils/aether_thread.h"
#include <stdint.h>

// Error codes
#define AETHER_SUCCESS 0
#define AETHER_ERROR_OUT_OF_MEMORY -1
#define AETHER_ERROR_INVALID_ARGUMENT -2
#define AETHER_ERROR_NOT_INITIALIZED -3

// Runtime configuration flags
#define AETHER_FLAG_AUTO_DETECT      (1 << 0)  // Auto-detect CPU features (recommended)
#define AETHER_FLAG_LOCKFREE_MAILBOX (1 << 1)  // Use lock-free SPSC mailboxes
#define AETHER_FLAG_LOCKFREE_POOLS   (1 << 2)  // Use lock-free TLS message pools
#define AETHER_FLAG_ENABLE_SIMD      (1 << 3)  // Enable AVX2 vectorization
#define AETHER_FLAG_ENABLE_MWAIT     (1 << 4)  // Enable MWAIT for idle
#define AETHER_FLAG_VERBOSE          (1 << 5)  // Print configuration on init

// Runtime configuration structure
typedef struct {
    int num_cores;
    int flags;
    int use_lockfree_mailbox;
    int use_lockfree_pools;
    int use_mwait;
    int use_simd;
} AetherRuntimeInitConfig;

// Runtime initialization
void aether_runtime_init(int num_cores, int flags);
void aether_runtime_shutdown();

// Command-line arguments (set by main, accessible from anywhere)
extern int aether_argc;
extern char** aether_argv;
void aether_args_init(int argc, char** argv);
int aether_args_count(void);
const char* aether_args_get(int index);

// Reclaim a closure environment through the destructor stored in its first
// field. Safe on NULL and on an env with no destructor. #1398
void aether_closure_env_free(void* env);

// Return argv[0] — the path the OS launched the current process with.
// Borrowed pointer into the OS argv storage; do not free. Returns NULL
// when aether_args_init has not been called.
const char* aether_argv0(void);

// Return the raw argv vector captured by aether_args_init. Borrowed pointer
// into the OS argv storage; do not free or retain beyond process lifetime.
char** aether_argv_raw(void);

/* One-shot runtime seal of the argv accessors. After this returns,
 * aether_args_count() / aether_args_get() / aether_argv0() /
 * aether_argv_raw() behave as if argv had never been initialised
 * (returning 0 / NULL respectively); the internal pointer is cleared
 * so a later peek can't recover it. Idempotent — calling it twice is
 * a no-op. There is no unseal.
 *
 * Intended use: after main() has finished consuming command-line
 * arguments (parsing flags into config, etc.), call os.args_seal()
 * to prevent any later code — including imported libraries, plugin
 * callbacks, or untrusted Aether modules running inside the same
 * process — from peeking at the original argv. Complements the
 * compile-time `hide` / `seal except` scope directives, which deny
 * lexical access; this denies runtime access.
 *
 * Caveat: this is a co-operative Aether-side gate, not a kernel
 * boundary. The OS still has the original argv in process memory
 * (/proc/self/cmdline on Linux, sysctl on macOS) and code that
 * goes around the Aether accessors can still read it. The seal
 * defends against accidental late-stage Aether code touching argv,
 * not against an attacker with arbitrary execution. Pair with the
 * LD_PRELOAD libc sandbox if the threat model requires true
 * inaccessibility. */
void aether_args_seal(void);
int  aether_args_sealed(void);

// Configuration queries
const AetherRuntimeInitConfig* aether_runtime_get_config();
int aether_runtime_has_feature(int feature_flag);
void aether_runtime_print_config();

// Aether built-in `sleep(ms)` lowers to a call to this — a stable,
// prefixed symbol that won't collide with libc's `sleep` if user code
// declares `extern sleep(...)` for some other purpose. See issue #233.
void aether_sleep_ms(int ms);

// Legacy compatibility
static inline void aether_init() {
    aether_runtime_init(0, AETHER_FLAG_AUTO_DETECT);
}

static inline void aether_cleanup() {
    aether_runtime_shutdown();
}

#endif
