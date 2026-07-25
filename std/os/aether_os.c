#include "aether_os.h"
#include "../mem/aether_grow.h"
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/aether_sandbox.h"
#include "../../runtime/aether_resource_caps.h"
#include <stdlib.h>

/* Heap-empty sentinel for the position-0 slot on error returns
 * from extern functions annotated `(string @heap, ...)` —
 * specifically os_run_capture_status_raw and
 * os_run_pipe_drain_and_wait_raw. The caller-side heap-string-
 * tracker auto-frees position 0 on function exit; returning a
 * static literal "" would surface as `free((void*)"")` and abort.
 * Allocate a fresh 1-byte buffer so the @heap contract holds
 * uniformly across success and error paths. NULL on OOM is
 * acceptable — `free(NULL)` is a defined no-op. See #420 v2. */
static char* aether_os_empty_heap(void) {
    char* p = (char*)malloc(1);
    if (p) p[0] = '\0';
    return p;
}

// NOTE on aether_argv0 placement: the implementation lives in
// runtime/aether_runtime.c next to the aether_argc / aether_argv it
// reads. aether_os.c cannot reference those variables directly because
// the compiler binary (aetherc) links std/os/aether_os.c but NOT
// runtime/aether_runtime.c, so a hard reference would break the
// compiler link. Keeping the function next to its state fixes that and
// leaves this file focused on shell/exec helpers.

#if !AETHER_HAS_FILESYSTEM
int os_system(const char* c) { (void)c; return -1; }
char* os_exec_raw(const char* c) { (void)c; return NULL; }
char* os_getenv(const char* n) { (void)n; return NULL; }
int os_execv(const char* p, void* a) { (void)p; (void)a; return -1; }
char* os_which(const char* n) { (void)n; return NULL; }
int os_chdir_raw(const char* p) { (void)p; return -1; }
char* os_getcwd_raw(void) { return NULL; }
int os_run(const char* p, void* a, void* e) { (void)p; (void)a; (void)e; return -1; }
char* os_run_capture_raw(const char* p, void* a, void* e) { (void)p; (void)a; (void)e; return NULL; }
typedef struct { const char* _0; int _1; const char* _2; } _tuple_string_int_string;
_tuple_string_int_string os_run_capture_status_raw(const char* p, void* a, void* e) {
    (void)p; (void)a; (void)e;
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "os.run_capture unavailable" };
    return out;
}
typedef struct { int _0; int _1; const char* _2; } _tuple_int_int_string;
typedef struct { int _0; const char* _1; } _tuple_int_string;
_tuple_int_int_string os_run_pipe_raw(const char* p, void* a, void* e) {
    (void)p; (void)a; (void)e;
    _tuple_int_int_string out = { -1, -1, "os.run_pipe unavailable" };
    return out;
}
_tuple_int_string os_wait_pid_raw(int pid) {
    (void)pid;
    _tuple_int_string out = { -1, "os.wait_pid unavailable" };
    return out;
}
_tuple_int_string os_spawn_raw(const char* p, void* a, void* e) {
    (void)p; (void)a; (void)e;
    _tuple_int_string out = { -1, "os.spawn unavailable" };
    return out;
}
_tuple_int_string os_wait_raw(int token) {
    (void)token;
    _tuple_int_string out = { -1, "os.wait unavailable" };
    return out;
}
_tuple_int_int_string os_wait_any_raw(void* token_list) {
    (void)token_list;
    _tuple_int_int_string out = { -1, -1, "os.wait_any unavailable" };
    return out;
}
int os_kill_raw(int pid, int sig) { (void)pid; (void)sig; return -1; }
_tuple_int_int_string os_wait_pid_timeout_raw(int pid, int secs) {
    (void)pid; (void)secs;
    _tuple_int_int_string out = { -1, 0, "os.wait_pid_timeout unavailable" };
    return out;
}
_tuple_int_string os_run_supervised_raw(const char* p, void* a, void* e,
                                        int g, int f, int t, int r) {
    (void)p; (void)a; (void)e; (void)g; (void)f; (void)t; (void)r;
    _tuple_int_string out = { -1, "os.run_supervised unavailable" };
    return out;
}
typedef struct { const char* _0; const char* _1; int _2; const char* _3; } _tuple_string_string_int_string;
_tuple_string_string_int_string os_run_full_raw(const char* p, void* a, void* e,
                                                const char* in, int in_len) {
    (void)p; (void)a; (void)e; (void)in; (void)in_len;
    _tuple_string_string_int_string out = {
        aether_os_empty_heap(), aether_os_empty_heap(), -1, "os.run_full unavailable" };
    return out;
}
_tuple_string_int_string os_run_pipe_drain_and_wait_raw(const char* p, void* a, void* e) {
    (void)p; (void)a; (void)e;
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "os.run_pipe_drain_and_wait unavailable" };
    return out;
}
/* ipc_parent_channel_raw — implemented in std/ipc/aether_ipc.c
 * (its no-FS stub returns -1 there). */
char* os_now_utc_iso8601_raw(void) { return NULL; }
char* os_now_local_iso8601_raw(void) { return NULL; }
void os_now_local_fill_raw(void* out) { (void)out; }
char* os_platform_raw(void) { return NULL; }
int os_getpid_raw(void) { return 0; }
int64_t os_wall_seconds_raw(void) { return 0; }
int os_wall_micros_raw(void) { return 0; }
int64_t os_now_monotonic_ms_raw(void) { return 0; }
int64_t os_now_monotonic_ns_raw(void) { return 0; }
int64_t os_now_unix_ms_raw(void) { return 0; }
#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

// Forward declarations for the Aether collections API. aether_os.c sits
// below std/collections in the link order, so we can't include the header
// here without a dependency cycle; the prototypes match
// std/collections/aether_collections.h.
extern int list_size(void* list);
extern void* list_get_raw(void* list, int index);

// String-ABI accessor: list_get_raw returns either an AetherString*
// (heap-built strings — concat, substring, interp, etc.) or a raw
// const char* (string literals, args_get). aether_string_data dispatches
// on the magic header and returns a plain C-string in both cases. Argv
// / envp arrays MUST go through this — the execve(2) family expects
// const char*, and a blind (char*)cast of a magic AetherString* yields
// the struct header bytes, not the payload (issue #688).
extern const char* aether_string_data(const void* s);

#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>  /* fcntl, F_GETFD — used by ipc_parent_channel_raw */
#include <signal.h> /* kill, sigaction — process-supervision primitives */
#include <time.h>   /* nanosleep, struct timespec — bounded waits */
#include <poll.h>   /* poll — deadlock-free 3-way stdio pump in os_run_full_raw */
#else
#include <windows.h>
#include <wchar.h>
#include <process.h>  /* _getpid */
#endif

// libaether.a list operations — declared extern so this file doesn't have
// to include the collections header (which would create a build cycle).
extern int   list_size(void* list);
extern void* list_get_raw(void* list, int index);
extern const char* aether_string_data(const void* s);

#ifdef _WIN32
// Forward declaration — implementation at the bottom of the file.
// os_execv needs to call into this before the Windows backend block.
static int win_launch(const char* prog, void* argv_list, void* env_list,
                      int capture_stdout,
                      int* out_exit_code, char** out_capture);
#endif

int os_system(const char* cmd) {
    if (!cmd) return -1;
    if (!aether_sandbox_check("exec", cmd)) return -1;
    int status = system(cmd);
    if (status == -1) return -1;
#ifdef _WIN32
    /* Windows's system() already returns the exit code directly. */
    return status;
#else
    /* POSIX system() returns a wait-status word: low byte = signal,
     * high byte = exit code on normal exit. Aether's contract (per
     * docs/stdlib-reference.md) is "returns exit code" — match the
     * shape of os_run's exit-code handling (line ~493): normal
     * termination → WEXITSTATUS, signal → 128 + signum (shell
     * convention), abnormal → -1. Pre-fix, callers had to `>> 8`
     * by hand on POSIX and not on Windows; that's a cross-platform
     * inconsistency the contract didn't promise. */
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}

char* os_exec_raw(const char* cmd) {
    if (!cmd) return NULL;
    if (!aether_sandbox_check("exec", cmd)) return NULL;

#ifdef _WIN32
    FILE* pipe = _popen(cmd, "r");
#else
    FILE* pipe = popen(cmd, "r");
#endif
    if (!pipe) return NULL;

    size_t capacity = 1024;
    size_t len = 0;
    /* #462: gate the command-output buffer through the capability
     * allocator so a sandboxed plugin can't inflate it without bound.
     * Caller-owned return — the success path hands `result` to the
     * Aether heap-string tracker, which frees it with libc free; that
     * leaves the cap counter drifted up by `capacity` (fail-safe — it
     * never under-counts), the same documented contract as
     * io_read_file_raw / os_getenv. Error paths below free through the
     * cap so the counter stays balanced when nothing escapes. */
    char* result = (char*)aether_caps_malloc(capacity);
    if (!result) {
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        return NULL;
    }

    // fgets reads up to sizeof(buffer)-1 per call, so a single chunk
    // won't need more than one doubling — but use `while` anyway to
    // stay safe against future buffer-size changes.
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        size_t chunk = strlen(buffer);
        if (len + chunk + 1 > capacity) {
            size_t new_capacity = capacity;
            while (new_capacity < len + chunk + 1) new_capacity *= 2;
            char* new_result = (char*)aether_caps_realloc(result, capacity, new_capacity);
            if (!new_result) {
                aether_caps_free(result, capacity);
#ifdef _WIN32
                _pclose(pipe);
#else
                pclose(pipe);
#endif
                return NULL;
            }
            result = new_result;
            capacity = new_capacity;
        }
        memcpy(result + len, buffer, chunk);
        len += chunk;
    }

    result[len] = '\0';

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

char* os_getenv(const char* name) {
    if (!name) return NULL;
    if (!aether_sandbox_check("env", name)) return NULL;
    char* val = getenv(name);
    if (!val) return NULL;
    /* #462: gate through the cap (a plugin can't read an unbounded
     * environment value past the limit). Caller-owned return freed by
     * libc free — fail-safe upward drift, identical to io_getenv. */
    size_t n = strlen(val);
    char* out = (char*)aether_caps_malloc(n + 1);
    if (out) memcpy(out, val, n + 1);
    return out;
}

/* Format the current UTC time as an ISO-8601 timestamp string:
 * "YYYY-MM-DDThh:mm:ssZ" (20 chars + NUL). Returns a malloc'd
 * strdup. Uses gmtime_r for thread safety. On any clock / format
 * failure returns an empty string — callers that care about the
 * distinction should check for "" vs a valid timestamp shape.
 *
 * Sub-second precision, timezone offsets, and format flags are
 * out of scope for v1 — keep it to the one shape callers wanting
 * a "timestamp this event happened" field reach for. Additive
 * variants (`now_utc_iso8601_ms`, `format_time`) can land without
 * breaking this one. */
char* os_now_utc_iso8601_raw(void) {
    time_t now = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    if (gmtime_s(&tm_buf, &now) != 0) return strdup("");
#else
    if (!gmtime_r(&now, &tm_buf)) return strdup("");
#endif
    char buf[32];
    /* "YYYY-MM-DDThh:mm:ssZ" — 20 bytes + NUL, fits buf easily. */
    if (strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm_buf) == 0) {
        return strdup("");
    }
    return strdup(buf);
}

/* ---- Local wall-clock time ------------------------------------------
 *
 * Aether's `now_local()` allocates a `LocalTime` struct and hands it
 * to `os_now_local_fill_raw` to populate. ONE `gettimeofday` +
 * `localtime_r` call per Aether call — fields are atomic with each
 * other.
 *
 * The companion `os_now_local_iso8601_raw` formats the same data into
 * an RFC-3339 / ISO-8601 string with explicit timezone offset
 * (`"YYYY-MM-DDThh:mm:ss±HH:MM"`, or `"...Z"` when offset == 0).
 *
 * Timezone offset: tm_gmtoff is GNU/BSD; on Windows the offset is
 * derived via `_get_timezone` (note: opposite sign from gmtoff —
 * _timezone is "seconds WEST of UTC", gmtoff is "seconds EAST"). DST
 * adjustment on Windows: `localtime_s` writes `tm_isdst`; we add
 * 3600 seconds when DST is active.
 *
 * The C-side AetherOsLocalTime struct mirrors the Aether-side
 * `struct LocalTime` in std/os/module.ae exactly: eight `int`
 * fields, no padding. Layout is asserted at compile time. */
typedef struct AetherOsLocalTime {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int nanos;
    int tz_offset_minutes;
} AetherOsLocalTime;

/* Layout sanity — every consumer mallocs sizeof(LocalTime) on the
 * Aether side; this asserts the C struct matches that assumption. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(AetherOsLocalTime) == 8 * sizeof(int),
               "AetherOsLocalTime must be 8 packed ints to match Aether's struct LocalTime");
#endif

#ifdef _WIN32
static int aether_os_local_tz_offset_seconds(const struct tm* tm_buf) {
    long tz_seconds_west = 0;
    _get_timezone(&tz_seconds_west);
    int offset = (int)(-tz_seconds_west);  /* convert "west" to "east" */
    if (tm_buf && tm_buf->tm_isdst > 0) offset += 3600;
    return offset;
}
#endif

/* Forward decl — defined further down next to the wall-clock raw
 * getters. We call it from os_now_local_fill_raw to read seconds +
 * usec in one shot. */
static void aether_os_wall_now(int64_t *psec, int *pusec);

/* Fill an Aether-allocated LocalTime struct with the current local
 * wall-clock time. The Aether wrapper allocates sizeof(LocalTime),
 * passes its ptr here. The void* signature matches the public header
 * (keeps AetherOsLocalTime an internal detail); we cast back inside.
 * On failure (clock read / localtime conversion) the struct is
 * zeroed; the wrapper surfaces year == 0 as the failure signal. */
void os_now_local_fill_raw(void* out_ptr) {
    if (!out_ptr) return;
    AetherOsLocalTime* out = (AetherOsLocalTime*)out_ptr;
    int64_t sec; int usec;
    aether_os_wall_now(&sec, &usec);
    time_t t = (time_t)sec;
    struct tm tm_buf;
#ifdef _WIN32
    if (localtime_s(&tm_buf, &t) != 0) {
        memset(out, 0, sizeof *out);
        return;
    }
    int tz_seconds = aether_os_local_tz_offset_seconds(&tm_buf);
#else
    if (!localtime_r(&t, &tm_buf)) {
        memset(out, 0, sizeof *out);
        return;
    }
    int tz_seconds = (int)tm_buf.tm_gmtoff;
#endif
    out->year              = tm_buf.tm_year + 1900;
    out->month             = tm_buf.tm_mon + 1;
    out->day               = tm_buf.tm_mday;
    out->hour              = tm_buf.tm_hour;
    out->minute            = tm_buf.tm_min;
    out->second            = tm_buf.tm_sec;
    out->nanos             = usec * 1000;
    out->tz_offset_minutes = tz_seconds / 60;
}

/* RFC-3339 / ISO-8601 with explicit timezone offset:
 *   "YYYY-MM-DDThh:mm:ss+HH:MM"   (positive offset)
 *   "YYYY-MM-DDThh:mm:ss-HH:MM"   (negative offset)
 *   "YYYY-MM-DDThh:mm:ssZ"        (offset == 0, UTC-equivalent)
 * 25 chars + NUL maximum. Returns strdup'd; "" on failure. */
char* os_now_local_iso8601_raw(void) {
    AetherOsLocalTime lt;
    os_now_local_fill_raw(&lt);
    if (lt.year == 0) return strdup("");  /* localtime failed */
    /* 64 bytes is generous — the longest legitimate output is 25 chars
     * + NUL, and oversizing here avoids gcc's -Wformat-truncation
     * cautious worst-case analysis (it can't prove the int args fit
     * in 4 / 2 digits respectively). */
    char buf[64];
    if (lt.tz_offset_minutes == 0) {
        snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 lt.year, lt.month, lt.day, lt.hour, lt.minute, lt.second);
    } else {
        int abs_min = lt.tz_offset_minutes < 0
                          ? -lt.tz_offset_minutes
                          : lt.tz_offset_minutes;
        snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
                 lt.year, lt.month, lt.day, lt.hour, lt.minute, lt.second,
                 lt.tz_offset_minutes < 0 ? '-' : '+',
                 abs_min / 60, abs_min % 60);
    }
    return strdup(buf);
}

/* Platform name as a flat lowercase string, matching the Go/Rust
 * convention (Go `runtime.GOOS`, Rust `std::env::consts::OS`):
 *
 *   "linux"      Linux kernel (any libc)
 *   "darwin"     macOS / iOS / any Darwin kernel
 *   "windows"    Windows (any toolchain: MSVC, MinGW, MSYS2)
 *   "freebsd"    FreeBSD
 *   "openbsd"    OpenBSD
 *   "netbsd"     NetBSD
 *   "dragonfly"  DragonFly BSD
 *   "solaris"    Solaris / illumos
 *   "wasm"       WebAssembly (Emscripten or wasi)
 *   "unknown"    None of the above matched at compile time
 *
 * Picked at compile time via the toolchain's standard predefined
 * macros (__APPLE__, __linux__, _WIN32, etc.). The returned string
 * is a strdup'd copy so callers can assign and the runtime's heap-
 * string tracker can free uniformly. */
char* os_platform_raw(void) {
#if defined(_WIN32) || defined(_WIN64)
    return strdup("windows");
#elif defined(__APPLE__)
    return strdup("darwin");
#elif defined(__linux__)
    return strdup("linux");
#elif defined(__FreeBSD__)
    return strdup("freebsd");
#elif defined(__OpenBSD__)
    return strdup("openbsd");
#elif defined(__NetBSD__)
    return strdup("netbsd");
#elif defined(__DragonFly__)
    return strdup("dragonfly");
#elif defined(__sun) && (defined(__SVR4) || defined(__svr4__))
    return strdup("solaris");
#elif defined(__EMSCRIPTEN__) || defined(__wasi__) || defined(__wasm__)
    return strdup("wasm");
#else
    return strdup("unknown");
#endif
}

/* Process identifier for the current process. Useful for tmpfile names
 * (`/tmp/myprog.${pid}.tmp`), per-process locks, log prefixes, and
 * stable tagging across forked children. POSIX uses `getpid(2)`;
 * Windows uses `_getpid()` from <process.h>. Returns an int across
 * both platforms — Windows PIDs fit in 32 bits even though the
 * GetCurrentProcessId() type is DWORD. Sandbox-free: this is a
 * pure-information call, not an action. */
int os_getpid_raw(void) {
#ifdef _WIN32
    return (int)_getpid();
#else
    return (int)getpid();
#endif
}

/* Wall-clock time split into whole seconds since the Unix epoch and
 * the sub-second microsecond fraction (0..999999).  A single
 * gettimeofday/filetime read fills a static cache so the two raw
 * accessors are consistent when called back-to-back. struct timeval /
 * FILETIME stay entirely C-side — Aether only ever sees int64/int. */
#ifdef _WIN32
#include <windows.h>
static void aether_os_wall_now(int64_t *psec, int *pusec) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    /* FILETIME: 100-ns ticks since 1601-01-01. Convert to Unix epoch. */
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;          /* 1601->1970 offset, 100ns units */
    *psec  = (int64_t)(t / 10000000ULL);
    *pusec = (int)((t / 10ULL) % 1000000ULL);
}
#else
#include <sys/time.h>
static void aether_os_wall_now(int64_t *psec, int *pusec) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) { *psec = 0; *pusec = 0; return; }
    *psec  = (int64_t)tv.tv_sec;
    *pusec = (int)tv.tv_usec;
}
#endif

int64_t os_wall_seconds_raw(void) {
    int64_t sec; int usec;
    aether_os_wall_now(&sec, &usec);
    return sec;
}

int os_wall_micros_raw(void) {
    int64_t sec; int usec;
    aether_os_wall_now(&sec, &usec);
    return usec;
}

/* Monotonic clock primitives. Wall-clock time (above) is NTP-jumpable
 * and tied to UTC; useless for animation tick loops, frame-time
 * budgets, or measuring elapsed time across a function call. These
 * use the per-platform monotonic source whose value-domain is opaque
 * (epoch is boot / process-start / arbitrary) but whose *deltas*
 * monotonically track real elapsed time.
 *
 * IMPORTANT: return type is int64_t, NOT C `long`. Aether's `long`
 * lowers to int64_t on every platform, but C `long` is 32-bit on
 * Windows (LLP64). Returning C `long` would truncate the upper 32
 * bits into the Aether 8-byte slot on Windows — the bug PR #562
 * caught and fixed for string_to_long_raw. Same hazard, same fix.
 *
 *   ms — milliseconds since boot/process-start. Wraparound at ~292
 *        million years from epoch; not a real concern.
 *   ns — nanoseconds. Useful for sub-ms timing (animation t-curves,
 *        microbenchmarks). Wraparound at ~292 years; also fine.
 */
#ifndef _WIN32
int64_t os_now_monotonic_ms_raw(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

int64_t os_now_monotonic_ns_raw(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

/* Wall-clock unix epoch ms. CLOCK_REALTIME because that's what every
 * other timestamp-consuming system on the network is using; the
 * NTP-step caveat is in the header. POSIX guarantees CLOCK_REALTIME;
 * the fallback to time(2) is for the (vanishingly rare) systems
 * where clock_gettime is broken but time(2) works. */
int64_t os_now_unix_ms_raw(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
    }
    return (int64_t)time(NULL) * 1000;
}
#else
/* Windows: QueryPerformanceCounter gives a high-resolution monotonic
 * counter; QueryPerformanceFrequency gives its tick rate. Frequency
 * is invariant across the OS lifetime (Win7+), so we cache it on first
 * call. Both functions are documented to always succeed on those
 * versions, so we treat a 0 frequency as the only failure mode. */
static LARGE_INTEGER qpc_freq_cache = {0};
static int qpc_freq_initialized = 0;

static int64_t qpc_freq_ticks_per_sec(void) {
    if (!qpc_freq_initialized) {
        if (!QueryPerformanceFrequency(&qpc_freq_cache)) return 0;
        qpc_freq_initialized = 1;
    }
    return (int64_t)qpc_freq_cache.QuadPart;
}

int64_t os_now_monotonic_ms_raw(void) {
    int64_t freq = qpc_freq_ticks_per_sec();
    if (freq == 0) return 0;
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter)) return 0;
    /* Compute ms as (ticks * 1000) / freq. Doing the multiply first
     * preserves sub-second precision; on a 10 MHz counter this loses
     * resolution at 2^53 ticks / 10^4 ≈ 28e8 seconds (90 years) which
     * is fine. For very-long-lived processes the ns variant has more
     * headroom because freq divides cleanly into 10^9 on x86. */
    return ((int64_t)counter.QuadPart * 1000) / freq;
}

int64_t os_now_monotonic_ns_raw(void) {
    int64_t freq = qpc_freq_ticks_per_sec();
    if (freq == 0) return 0;
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter)) return 0;
    /* Split-multiply to avoid overflowing int64 on long uptimes:
     * ticks*1e9/freq could overflow at ~2^53/1e9 ≈ 9e15 ticks (≈ 28
     * years on a 10 MHz counter). The split form: (ticks / freq) * 1e9
     * + ((ticks % freq) * 1e9 / freq) — exact, no overflow. */
    int64_t ticks = (int64_t)counter.QuadPart;
    int64_t whole_sec = ticks / freq;
    int64_t frac_ticks = ticks % freq;
    return whole_sec * 1000000000 + (frac_ticks * 1000000000) / freq;
}

/* GetSystemTimeAsFileTime: 100-ns intervals since 1601-01-01 UTC.
 * Convert to unix-ms by subtracting the 116444736000000000 epoch
 * offset and dividing by 10000. */
int64_t os_now_unix_ms_raw(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int64_t ticks = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (ticks - 116444736000000000LL) / 10000;
}
#endif

int os_execv(const char* prog, void* argv_list) {
    if (!prog) return -1;
    if (!aether_sandbox_check("exec", prog)) return -1;

#ifdef _WIN32
    // Windows has no true exec that replaces the process in place. The
    // closest semantic: spawn the child synchronously, inherit stdio,
    // and exit with the child's exit code so the caller sees the same
    // effective behavior as POSIX execvp (the original process is gone
    // after this call returns successfully — but technically returns
    // via exit, not via in-place replacement).
    int exit_code = 0;
    if (win_launch(prog, argv_list, NULL, 0, &exit_code, NULL) != 0) {
        return -1;
    }
    fflush(stdout);
    fflush(stderr);
    exit(exit_code);
    return -1;  // unreachable
#else
    // Build a NULL-terminated char* array from the Aether list. We copy
    // pointers only — the list owns the string storage and keeps it
    // alive for the duration of the call (which, on success, is the
    // rest of forever — the process image is replaced in place).
    int n = argv_list ? list_size(argv_list) : 0;

    // Guard against pathological sizes before multiplying for malloc.
    if (n < 0) return -1;
    if ((size_t)n > (SIZE_MAX / sizeof(char*)) - 2) return -1;

    /* #462: internal/transient argv scratch — alloc and free both live
     * in this function (or vanish into a successful execvp). Gate it and
     * free through the cap with the exact byte count so accounting is
     * balanced; the execvp-success path never returns, so its bytes are
     * reclaimed by the process-image replacement, not a leak. */
    size_t argv_bytes = sizeof(char*) * (size_t)(n + 2);
    char** argv = (char**)aether_caps_malloc(argv_bytes);
    if (!argv) return -1;

    // Canonical POSIX behaviour: argv[0] is the program name. If the
    // caller passed an empty list we synthesise argv[0] from `prog` so
    // the callee sees a sensible value; otherwise the caller owns the
    // whole argv and we trust their layout.
    int ai = 0;
    if (n == 0) {
        argv[ai++] = (char*)prog;
    } else {
        for (int i = 0; i < n; i++) {
            void* item = list_get_raw(argv_list, i);
            if (!item) {
                // Bail early rather than pass NULL into execvp's
                // variadic-argv contract, which has undefined behaviour
                // on many implementations.
                aether_caps_free(argv, argv_bytes);
                return -1;
            }
            // aether_string_data unwraps a magic AetherString* to its
            // payload pointer; for raw const char* it returns the input
            // verbatim. Required: a blind (char*)cast of a heap-built
            // string would give execvp the struct header (issue #688).
            argv[ai++] = (char*)aether_string_data(item);
        }
    }
    argv[ai] = NULL;

    // Flush stdio before replacing the process image. execvp destroys
    // the caller's stdio buffers, so anything println'd but not yet
    // flushed would be silently lost. Line-buffered output already on
    // a terminal is safe, but redirected / pipe output is typically
    // fully-buffered — without this flush, pre-exec diagnostics vanish.
    fflush(stdout);
    fflush(stderr);

    // execvp honours PATH if `prog` does not contain a slash. On
    // success this call never returns; on failure we free scratch and
    // report -1. We intentionally do not touch errno — callers that
    // want diagnostic detail should read it themselves after the call
    // via a dedicated wrapper (not exposed yet).
    execvp(prog, argv);
    aether_caps_free(argv, argv_bytes);
    return -1;
#endif
}

// Search PATH for an executable. POSIX semantics:
//   1. If `name` contains a '/', it's treated as a path (absolute or
//      relative to cwd). Return it as-is if executable, else NULL.
//   2. Otherwise iterate through colon-separated entries in $PATH (or a
//      sensible default if PATH isn't set), looking for `<dir>/<name>`
//      that's executable. Return the first hit.
//
// Caller owns the returned string.
char* os_which(const char* name) {
    if (!name || !*name) return NULL;
    if (!aether_sandbox_check("env", "PATH")) return NULL;

#ifdef _WIN32
    // Windows: PATH separator is ';', and executable extensions are
    // enumerated via PATHEXT (e.g. ".COM;.EXE;.BAT;.CMD"). Absolute or
    // relative paths with backslashes or drive letters are returned
    // as-is after existence check.
    size_t name_len = strlen(name);

    // Name already has a path component? (slash, backslash, or drive letter)
    int has_path = 0;
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == '\\' || name[i] == '/' || (i == 1 && name[i] == ':')) {
            has_path = 1;
            break;
        }
    }

    // Has an extension already?
    const char* dot = strrchr(name, '.');
    const char* last_sep = NULL;
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == '\\' || name[i] == '/') last_sep = name + i;
    }
    int has_ext = dot && (!last_sep || dot > last_sep);

    const char* pathext = getenv("PATHEXT");
    if (!pathext || !*pathext) pathext = ".COM;.EXE;.BAT;.CMD";

    // Helper: try a candidate path; return strdup of it if the file exists.
    // Inlined via macro since we use it in both the has-path and PATH-search branches.
    #define WIN_WHICH_TRY(candidate) do { \
        DWORD attrs = GetFileAttributesA(candidate); \
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) { \
            return strdup(candidate); \
        } \
    } while (0)

    char buf[MAX_PATH];

    if (has_path) {
        if (has_ext) {
            WIN_WHICH_TRY(name);
            return NULL;
        }
        // Try name with each PATHEXT extension.
        const char* p = pathext;
        while (*p) {
            const char* end = strchr(p, ';');
            size_t ext_len = end ? (size_t)(end - p) : strlen(p);
            if (name_len + ext_len + 1 < sizeof(buf)) {
                memcpy(buf, name, name_len);
                memcpy(buf + name_len, p, ext_len);
                buf[name_len + ext_len] = '\0';
                WIN_WHICH_TRY(buf);
            }
            if (!end) break;
            p = end + 1;
        }
        return NULL;
    }

    const char* path = getenv("PATH");
    if (!path || !*path) return NULL;

    const char* p = path;
    while (*p) {
        const char* end = strchr(p, ';');
        size_t dirlen = end ? (size_t)(end - p) : strlen(p);
        if (dirlen == 0 || dirlen + 1 + name_len >= sizeof(buf)) {
            if (!end) break;
            p = end + 1;
            continue;
        }
        memcpy(buf, p, dirlen);
        buf[dirlen] = '\\';
        memcpy(buf + dirlen + 1, name, name_len);
        buf[dirlen + 1 + name_len] = '\0';

        if (has_ext) {
            WIN_WHICH_TRY(buf);
        } else {
            char ext_buf[MAX_PATH];
            const char* ep = pathext;
            while (*ep) {
                const char* eend = strchr(ep, ';');
                size_t elen = eend ? (size_t)(eend - ep) : strlen(ep);
                size_t base_len = dirlen + 1 + name_len;
                if (base_len + elen + 1 < sizeof(ext_buf)) {
                    memcpy(ext_buf, buf, base_len);
                    memcpy(ext_buf + base_len, ep, elen);
                    ext_buf[base_len + elen] = '\0';
                    WIN_WHICH_TRY(ext_buf);
                }
                if (!eend) break;
                ep = eend + 1;
            }
        }

        if (!end) break;
        p = end + 1;
    }
    #undef WIN_WHICH_TRY
    return NULL;
#else
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) return strdup(name);
        return NULL;
    }

    const char* path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";

    size_t name_len = strlen(name);
    char buf[4096];
    const char* p = path;
    while (*p) {
        const char* end = strchr(p, ':');
        size_t dirlen = end ? (size_t)(end - p) : strlen(p);
        // Empty entry means current directory (POSIX).
        if (dirlen == 0) {
            // Guard: we write "./" (2 bytes) plus name_len+1 bytes
            // (including null terminator) starting at buf+2. The last
            // written byte is at index (2 + name_len), which must be
            // strictly less than sizeof(buf) for validity.
            if (2 + name_len < sizeof(buf)) {
                buf[0] = '.';
                buf[1] = '/';
                memcpy(buf + 2, name, name_len + 1);
                if (access(buf, X_OK) == 0) return strdup(buf);
            }
        } else if (dirlen + 1 + name_len < sizeof(buf)) {
            memcpy(buf, p, dirlen);
            buf[dirlen] = '/';
            memcpy(buf + dirlen + 1, name, name_len + 1);
            if (access(buf, X_OK) == 0) return strdup(buf);
        }
        if (!end) break;
        p = end + 1;
    }
    return NULL;
#endif
}


// --- os_run / os_run_capture: argv-based child process launch ---
//
// Both functions take an Aether list as the argv (and optional env)
// rather than a shell-string command line. There is no /bin/sh in the
// loop, so paths-with-spaces, $variables, |, ;, *, and other shell
// metacharacters in argv items are passed verbatim. This eliminates a
// large class of quoting bugs and makes the same Aether code portable
// to platforms without a POSIX shell.
//
// Implementation: POSIX uses fork + execvp + waitpid, with execve when
// an explicit env is provided. Windows uses CreateProcessW (see win_launch
// below), building an argv-quoted command line and capturing stdout through
// an inheritable pipe, so the same argv-based contract holds on both.

#ifndef _WIN32

/* Change the process working directory. POSIX chdir(2). Gated under the
 * "fs" capability bucket: it repositions every subsequent relative path
 * the process touches, so it's a filesystem-scoped action. */
int os_chdir_raw(const char* path) {
    if (!path) return -1;
    if (!aether_sandbox_check("fs", path)) return -1;
    return chdir(path) == 0 ? 0 : -1;
}

/* Current working directory as a fresh strdup'd buffer (caller owns).
 * Grows the buffer until getcwd(3) stops reporting ERANGE so deep paths
 * past PATH_MAX still resolve. NULL on any other error. */
char* os_getcwd_raw(void) {
    size_t cap = 4096;
    for (;;) {
        /* #462: gate the cwd buffer (a deep path is plugin-influenced).
         * Caller-owned return on success (libc free, fail-safe drift);
         * the ERANGE retry frees through the cap before doubling, so the
         * free size matches the live `cap`. */
        char* buf = (char*)aether_caps_malloc(cap);
        if (!buf) return NULL;
        if (getcwd(buf, cap)) return buf;
        aether_caps_free(buf, cap);
        if (errno != ERANGE) return NULL;
        if (cap > (size_t)1 << 20) return NULL; /* 1 MiB sanity ceiling */
        cap *= 2;
    }
}

// Build a NULL-terminated argv array from an Aether list. The first
// entry in argv[] is `prog`. Caller must free the returned array (the
// strings inside are pointers into the Aether list and must NOT be
// freed individually). Returns NULL on allocation failure.
static char** build_argv_array(const char* prog, void* argv_list) {
    int n = argv_list ? list_size(argv_list) : 0;
    char** av = (char**)malloc(sizeof(char*) * (size_t)(n + 2));
    if (!av) return NULL;
    av[0] = (char*)prog;
    for (int i = 0; i < n; i++) {
        // Unwrap magic AetherString* to its payload; pass plain char*
        // through unchanged. See aether_os.c header comment + issue #688.
        av[i + 1] = (char*)aether_string_data(list_get_raw(argv_list, i));
    }
    av[n + 1] = NULL;
    return av;
}

// Build a NULL-terminated environ array from an Aether list of
// "KEY=VALUE" strings. Returns NULL if env_list is NULL (caller should
// inherit parent env in that case). Caller must free the returned
// array.
static char** build_envp_array(void* env_list) {
    if (!env_list) return NULL;
    int n = list_size(env_list);
    char** envp = (char**)malloc(sizeof(char*) * (size_t)(n + 1));
    if (!envp) return NULL;
    for (int i = 0; i < n; i++) {
        envp[i] = (char*)aether_string_data(list_get_raw(env_list, i));
    }
    envp[n] = NULL;
    return envp;
}

int os_run(const char* prog, void* argv_list, void* env_list) {
    if (!prog) return -1;
    if (!aether_sandbox_check("exec", prog)) return -1;

    char** av = build_argv_array(prog, argv_list);
    if (!av) return -1;
    char** envp = build_envp_array(env_list);

    pid_t pid = fork();
    if (pid < 0) {
        free(av);
        free(envp);
        return -1;
    }
    if (pid == 0) {
        // Child
        if (envp) {
            execve(prog, av, envp);
        } else {
            execvp(prog, av);
        }
        // exec only returns on failure
        _exit(127);
    }
    // Parent
    free(av);
    free(envp);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

char* os_run_capture_raw(const char* prog, void* argv_list, void* env_list) {
    if (!prog) return NULL;
    if (!aether_sandbox_check("exec", prog)) return NULL;

    int pipefd[2];
    if (pipe(pipefd) != 0) return NULL;

    char** av = build_argv_array(prog, argv_list);
    if (!av) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }
    char** envp = build_envp_array(env_list);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(av);
        free(envp);
        return NULL;
    }
    if (pid == 0) {
        // Child: redirect stdout to pipe write end, close read end
        close(pipefd[0]);
        if (dup2(pipefd[1], 1) < 0) _exit(127);
        close(pipefd[1]);
        if (envp) {
            execve(prog, av, envp);
        } else {
            execvp(prog, av);
        }
        _exit(127);
    }
    // Parent: close write end, read until EOF
    close(pipefd[1]);
    free(av);
    free(envp);

    // Read buffer is page-sized for fewer syscalls on large output;
    // result buffer starts at 4 KB with doubling growth.
    size_t cap = 4096;
    size_t len = 0;
    /* #462: bound the captured child stdout — the prime DoS surface
     * (a sandboxed plugin can spawn a process that floods stdout).
     * Caller-owned return freed by libc free (fail-safe upward drift,
     * the io_read_file_raw contract); error paths free through the cap
     * with the live size so the counter stays balanced. */
    char* result = (char*)aether_caps_malloc(cap);
    if (!result) {
        close(pipefd[0]);
        // Reap the child so we don't leave a zombie
        int st = 0;
        waitpid(pid, &st, 0);
        return NULL;
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            aether_caps_free(result, cap);
            close(pipefd[0]);
            int st = 0;
            waitpid(pid, &st, 0);
            return NULL;
        }
        if (n == 0) break;
        if (len + (size_t)n + 1 > cap) {
            size_t old_cap = cap;
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char* bigger = (char*)aether_caps_realloc(result, old_cap, cap);
            if (!bigger) {
                /* realloc failed: `result` still holds old_cap bytes. */
                aether_caps_free(result, old_cap);
                close(pipefd[0]);
                int st = 0;
                waitpid(pid, &st, 0);
                return NULL;
            }
            result = bigger;
        }
        memcpy(result + len, buf, (size_t)n);
        len += (size_t)n;
    }
    result[len] = '\0';
    close(pipefd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return result;
}

/* Tuple-returning sibling of os_run_capture_raw — captures stdout AND
 * exposes the child's exit code. Returns (stdout, status, err):
 *   - stdout: same shape os_run_capture_raw returns. Empty string
 *             when the spawn fails.
 *   - status: WEXITSTATUS(st) on normal exit, -1 when the child was
 *             killed by a signal or the spawn itself failed.
 *   - err:    "" on successful spawn (regardless of exit code);
 *             non-empty only when the fork/exec couldn't run.
 *
 * This is the canonical entry point for callers that need to
 * distinguish "ran cleanly" from "ran but exited non-zero" (diff3,
 * grep, gcc, etc.). The plain `os_run_capture_raw` stays for callers
 * that don't care about status. Issue #289. */
typedef struct { const char* _0; int _1; const char* _2; } _tuple_string_int_string;

_tuple_string_int_string os_run_capture_status_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "" };
    if (!prog) {
        out._2 = "null prog";
        return out;
    }
    if (!aether_sandbox_check("exec", prog)) {
        out._2 = "denied by sandbox";
        return out;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        out._2 = "pipe failed";
        return out;
    }

    char** av = build_argv_array(prog, argv_list);
    if (!av) {
        close(pipefd[0]);
        close(pipefd[1]);
        out._2 = "argv build failed";
        return out;
    }
    char** envp = build_envp_array(env_list);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(av);
        free(envp);
        out._2 = "fork failed";
        return out;
    }
    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], 1) < 0) _exit(127);
        close(pipefd[1]);
        if (envp) execve(prog, av, envp);
        else      execvp(prog, av);
        _exit(127);
    }
    close(pipefd[1]);
    free(av);
    free(envp);

    size_t cap = 4096;
    size_t len = 0;
    /* #462: cap the captured child stdout (see os_run_capture_raw).
     * `result` escapes into out._0 (@heap tuple slot, caller frees via
     * libc — fail-safe drift); error paths free through the cap. */
    char* result = (char*)aether_caps_malloc(cap);
    if (!result) {
        close(pipefd[0]);
        int st = 0;
        waitpid(pid, &st, 0);
        out._2 = "alloc failed";
        return out;
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            aether_caps_free(result, cap);
            close(pipefd[0]);
            int st = 0;
            waitpid(pid, &st, 0);
            out._2 = "read failed";
            return out;
        }
        if (n == 0) break;
        if (len + (size_t)n + 1 > cap) {
            size_t old_cap = cap;
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char* bigger = (char*)aether_caps_realloc(result, old_cap, cap);
            if (!bigger) {
                aether_caps_free(result, old_cap);
                close(pipefd[0]);
                int st = 0;
                waitpid(pid, &st, 0);
                out._2 = "realloc failed";
                return out;
            }
            result = bigger;
        }
        memcpy(result + len, buf, (size_t)n);
        len += (size_t)n;
    }
    result[len] = '\0';
    close(pipefd[0]);

    int st = 0;
    if (waitpid(pid, &st, 0) < 0) {
        /* Free the empty-heap initializer before overwriting with
         * the captured-output buffer (#420 v2 — keeps the @heap
         * contract from leaking 1 byte per failed-waitpid call). */
        free((void*)out._0);
        out._0 = result;
        out._1 = -1;
        out._2 = "waitpid failed";
        return out;
    }
    free((void*)out._0);
    out._0 = result;
    if (WIFEXITED(st)) {
        out._1 = WEXITSTATUS(st);
    } else {
        /* Killed by signal, stopped, or otherwise abnormal — surface
         * as -1 and non-empty err so the caller can distinguish from
         * a real-but-non-zero exit code. */
        out._1 = -1;
        out._2 = "child terminated abnormally";
    }
    return out;
}

/* ============================================================
 * std.ipc — fd-inheritance back-channel for Aether ↔ Aether spawn.
 *
 * Aeb's `aether.driver_test` pattern: parent spawns a child that
 * needs to send back a structured payload (test results, build
 * telemetry, etc) richer than an exit code. Without this, every
 * consumer reinvents the marker-file pattern.
 *
 * Mechanism: parent opens a pipe, dup2's the write end to a chosen
 * fd (3 by convention, but the value goes into AETHER_IPC_FD so
 * the child reads the env var rather than hardcoding the number),
 * forks, exec's. Child can use ipc.parent_channel() to discover
 * the fd, write a payload, close.
 *
 * Three variants of the parent-side spawn:
 *
 *   os_run_pipe_raw()                  — async; returns (read_fd,
 *                                        pid, err). Caller MUST
 *                                        wait via os_wait_pid_raw,
 *                                        and reads concurrently to
 *                                        avoid pipe-buffer deadlock
 *                                        if the payload exceeds
 *                                        OS pipe size (16K macOS,
 *                                        64K Linux).
 *   os_run_pipe_drain_and_wait_raw()   — sync convenience; reads
 *                                        the channel to EOF and
 *                                        waits for child. Same
 *                                        shape as run_capture but
 *                                        captures the IPC fd
 *                                        instead of stdout. No
 *                                        deadlock regardless of
 *                                        payload size.
 *   os_wait_pid_raw()                  — companion to os_run_pipe;
 *                                        reaps child, returns exit.
 *
 * AETHER_IPC_FD is the contract; fd 3 is the default but the env
 * var is what the child reads. Sidesteps fd-allocation surprises
 * in shell intermediaries (e.g. `bash -c '<driver>'`).
 * ============================================================ */

typedef struct { int _0; int _1; const char* _2; } _tuple_int_int_string;
typedef struct { int _0; const char* _1; } _tuple_int_string;

/* Spawn child with a back-channel pipe at fd 3 (write end), set
 * AETHER_IPC_FD=3 in child's env. Returns (parent_read_fd,
 * child_pid, err). On success, err is "" and caller must:
 *   1. Read from parent_read_fd until EOF.
 *   2. Call os_wait_pid_raw(child_pid) to reap.
 *   3. Close parent_read_fd.
 *
 * Failure modes return (-1, -1, "<reason>") with no spawn done.
 *
 * Pipe-buffer deadlock window: if child writes > OS pipe buffer
 * (typically 16K-64K) without parent reading concurrently, the
 * child blocks forever on its write. For payload-at-exit usage
 * with large payloads, prefer os_run_pipe_drain_and_wait_raw. */
_tuple_int_int_string os_run_pipe_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_int_int_string out = { -1, -1, "" };
    if (!prog) {
        out._2 = "null prog";
        return out;
    }
    if (!aether_sandbox_check("exec", prog)) {
        out._2 = "denied by sandbox";
        return out;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        out._2 = "pipe failed";
        return out;
    }

    char** av = build_argv_array(prog, argv_list);
    if (!av) {
        close(pipefd[0]);
        close(pipefd[1]);
        out._2 = "argv build failed";
        return out;
    }
    char** envp = build_envp_array(env_list);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(av);
        free(envp);
        out._2 = "fork failed";
        return out;
    }
    if (pid == 0) {
        /* Child — wire pipefd[1] to fd 3 and tell child where it is. */
        close(pipefd[0]);
        if (dup2(pipefd[1], 3) < 0) _exit(127);
        /* Close the original write end if dup2 didn't already (it
         * does when pipefd[1] != 3; the conditional close is a
         * defensive no-op when dup2 was a self-dup). */
        if (pipefd[1] != 3) close(pipefd[1]);
        /* AETHER_IPC_FD tells the child via env that fd 3 carries
         * the parent channel. The child's ipc.parent_channel()
         * reads this and returns the int — a future change can
         * pick a different fd by setting a different value, no
         * code change needed in the child. */
        setenv("AETHER_IPC_FD", "3", 1);
        if (envp) {
            /* If the caller supplied an explicit envp, AETHER_IPC_FD
             * needs to be present in it — setenv above only updates
             * the current process's environ, but execve will use
             * envp, not environ. Build augmented envp.
             *
             * For now, error if explicit envp is supplied; future
             * work can splice AETHER_IPC_FD into the supplied envp
             * before execve. Most aeb consumers pass null env. */
            _exit(127);
        } else {
            execvp(prog, av);
        }
        _exit(127);
    }
    close(pipefd[1]);
    free(av);
    free(envp);

    out._0 = pipefd[0];
    out._1 = pid;
    return out;
}

/* Wait for a child, return (exit_code, err). Reaps the child so
 * it doesn't become a zombie. Companion to os_run_pipe_raw. */
_tuple_int_string os_wait_pid_raw(int pid) {
    _tuple_int_string out = { -1, "" };
    if (pid <= 0) {
        out._1 = "invalid pid";
        return out;
    }
    int st = 0;
    if (waitpid((pid_t)pid, &st, 0) < 0) {
        out._1 = "waitpid failed";
        return out;
    }
    if (WIFEXITED(st)) {
        out._0 = WEXITSTATUS(st);
    } else {
        out._0 = -1;
        out._1 = "child terminated abnormally";
    }
    return out;
}

/* ============================================================
 * Non-blocking spawn + reap, WITHOUT the IPC back-channel pipe.
 *
 * These are the fan-out/fan-in pair a parallel build scheduler needs:
 * start up to N ready children, wait for ANY to finish, reap it, start
 * more. Unlike os_run_pipe_raw they create no pipe and set no
 * AETHER_IPC_FD — which is exactly why they are cross-platform (the pipe
 * is the part that needs coordinated _open_osfhandle on Windows). The
 * "token" returned by spawn is the reap handle: on POSIX it is the real
 * pid; on Windows it is an index into a process-handle table.
 *
 *   os_spawn_raw()     -> (token, err)       async spawn, no pipe
 *   os_wait_raw()      -> (exit, err)        reap one token
 *   os_wait_any_raw()  -> (token, exit, err) reap whichever finishes first
 *
 * Each spawned token MUST be reaped exactly once (via wait or wait_any)
 * or it leaks a zombie (POSIX) / handle (Windows). ============ */

/* Map a waitpid status to (exit, err), shared by wait and wait_any. */
static _tuple_int_string posix_status_to_tuple(int st) {
    _tuple_int_string out = { -1, "" };
    if (WIFEXITED(st)) {
        out._0 = WEXITSTATUS(st);
    } else {
        out._0 = -1;
        out._1 = "child terminated abnormally";
    }
    return out;
}

_tuple_int_string os_spawn_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_int_string out = { -1, "" };
    if (!prog) { out._1 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._1 = "denied by sandbox"; return out; }

    char** av = build_argv_array(prog, argv_list);
    if (!av) { out._1 = "argv build failed"; return out; }
    char** envp = build_envp_array(env_list);

    pid_t pid = fork();
    if (pid < 0) {
        free(av); free(envp);
        out._1 = "fork failed";
        return out;
    }
    if (pid == 0) {
        /* Child — no pipe, no fd 3, no AETHER_IPC_FD. Plain exec. */
        if (envp) {
            execve(prog, av, envp);
            /* execve doesn't PATH-resolve; fall back to execvp so a bare
             * program name still works when an explicit env was supplied. */
            execvp(prog, av);
        } else {
            execvp(prog, av);
        }
        _exit(127);
    }
    free(av);
    free(envp);
    out._0 = (int)pid;   /* token == pid on POSIX */
    return out;
}

_tuple_int_string os_wait_raw(int token) {
    /* On POSIX the token IS the pid, so reap semantics match wait_pid. */
    _tuple_int_string out = { -1, "" };
    if (token <= 0) { out._1 = "invalid token"; return out; }
    int st = 0;
    if (waitpid((pid_t)token, &st, 0) < 0) { out._1 = "waitpid failed"; return out; }
    return posix_status_to_tuple(st);
}

_tuple_int_int_string os_wait_any_raw(void* token_list) {
    _tuple_int_int_string out = { -1, -1, "" };
    int n = token_list ? list_size(token_list) : 0;
    if (n <= 0) { out._2 = "no tokens"; return out; }

    /* On POSIX the token IS the pid, and waitpid(-1) already reports which
     * child finished — so the returned pid is itself the finished token. We
     * don't unbox the list here (int-list element boxing is codegen-private
     * and not part of this file's ABI); the Windows backend, which cannot
     * wait on "any child" generically, is the one that consumes the list to
     * build its HANDLE array. A scheduler that spawns only via os_spawn_raw
     * has no foreign children, so the reaped pid is always one of its
     * tokens. */
    int st = 0;
    pid_t got = waitpid(-1, &st, 0);
    if (got < 0) { out._2 = "waitpid failed"; return out; }

    _tuple_int_string s = posix_status_to_tuple(st);
    out._0 = (int)got;      /* which token finished */
    out._1 = s._0;          /* its exit code */
    out._2 = s._1;          /* err (abnormal-termination note, if any) */
    return out;
}

/* Convenience: spawn + drain pipe + wait. Reads the back-channel
 * to EOF (driven by the child closing fd 3 or exiting) while the
 * child runs, then waits for the child to exit. Returns the
 * channel payload as a string, the exit code, and an error string.
 *
 * No deadlock risk: the read loop drains continuously, so the
 * pipe buffer never fills regardless of payload size. */
_tuple_string_int_string os_run_pipe_drain_and_wait_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "" };
    /* Delegate to os_run_pipe_raw for the spawn, then drain + wait. */
    _tuple_int_int_string spawn = os_run_pipe_raw(prog, argv_list, env_list);
    if (spawn._2 && spawn._2[0]) {
        out._2 = spawn._2;
        return out;
    }
    int read_fd = spawn._0;
    int pid = spawn._1;

    size_t cap = 4096;
    size_t len = 0;
    /* #462: cap the drained IPC-channel payload (see os_run_capture_raw).
     * `result` escapes into out._0 (@heap, caller frees via libc —
     * fail-safe drift); error paths free through the cap. */
    char* result = (char*)aether_caps_malloc(cap);
    if (!result) {
        close(read_fd);
        int st = 0;
        waitpid((pid_t)pid, &st, 0);
        out._2 = "alloc failed";
        return out;
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(read_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            aether_caps_free(result, cap);
            close(read_fd);
            int st = 0;
            waitpid((pid_t)pid, &st, 0);
            out._2 = "read failed";
            return out;
        }
        if (n == 0) break;
        if (len + (size_t)n + 1 > cap) {
            size_t old_cap = cap;
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char* bigger = (char*)aether_caps_realloc(result, old_cap, cap);
            if (!bigger) {
                aether_caps_free(result, old_cap);
                close(read_fd);
                int st = 0;
                waitpid((pid_t)pid, &st, 0);
                out._2 = "realloc failed";
                return out;
            }
            result = bigger;
        }
        memcpy(result + len, buf, (size_t)n);
        len += (size_t)n;
    }
    result[len] = '\0';
    close(read_fd);

    int st = 0;
    if (waitpid((pid_t)pid, &st, 0) < 0) {
        /* Free the empty-heap initializer before overwriting with
         * the drained payload (#420 v2). */
        free((void*)out._0);
        out._0 = result;
        out._1 = -1;
        out._2 = "waitpid failed";
        return out;
    }
    free((void*)out._0);
    out._0 = result;
    if (WIFEXITED(st)) {
        out._1 = WEXITSTATUS(st);
    } else {
        out._1 = -1;
        out._2 = "child terminated abnormally";
    }
    return out;
}

/* ============================================================
 * Process-supervision primitives — process groups, killpg, bounded
 * waits, and a one-call supervised run. Filed by aeb
 * (aeb-process-supervision-primitives.md): the floor any build/test
 * orchestrator hits — run a child as its own process group, forward
 * interactive signals to it, enforce a wall-clock timeout, then
 * group-reap anything the child leaked. POSIX-only; the Windows
 * branch stubs these as "unsupported".
 * ============================================================ */

/* Send `sig` to `pid`. Thin POSIX kill(2) wrapper, returning 0 on
 * success and -1 on failure. The two non-obvious conventions are
 * intentional and documented in the header / module.ae:
 *   - negative pid → signal the whole group abs(pid);
 *   - sig == 0     → existence/permission probe, no delivery.
 * Gated under the "exec" capability bucket: sending a signal is a
 * process-control action of the same blast radius as spawning one. */
int os_kill_raw(int pid, int sig) {
    if (!aether_sandbox_check("exec", "kill")) return -1;
    return kill((pid_t)pid, sig) == 0 ? 0 : -1;
}

/* Bounded wait for a single child. Returns (status, timed_out, err):
 *   - status:    WEXITSTATUS on normal exit, 128+signo when killed by
 *                a signal, -1 on timeout or error.
 *   - timed_out: 1 if `secs` elapsed before the child exited (the
 *                child is left running — the caller decides whether to
 *                kill it), 0 otherwise.
 *   - err:       "" on a clean reap or a clean timeout; non-empty on a
 *                waitpid error / invalid pid.
 * secs <= 0 means "block indefinitely" (equivalent to os_wait_pid_raw
 * but with the timed_out slot always 0). Poll cadence is 20ms against
 * a CLOCK_MONOTONIC deadline so a wall-clock jump can't distort it. */
_tuple_int_int_string os_wait_pid_timeout_raw(int pid, int secs) {
    _tuple_int_int_string out = { -1, 0, "" };
    if (pid <= 0) { out._2 = "invalid pid"; return out; }

    int st = 0;
    if (secs <= 0) {
        while (waitpid((pid_t)pid, &st, 0) < 0) {
            if (errno != EINTR) { out._2 = "waitpid failed"; return out; }
        }
    } else {
        int64_t deadline = os_now_monotonic_ms_raw() + (int64_t)secs * 1000;
        int reaped = 0;
        for (;;) {
            pid_t r = waitpid((pid_t)pid, &st, WNOHANG);
            if (r == (pid_t)pid) { reaped = 1; break; }
            if (r < 0) {
                if (errno == EINTR) continue;
                out._2 = "waitpid failed";
                return out;
            }
            if (os_now_monotonic_ms_raw() >= deadline) { out._1 = 1; return out; }
            struct timespec ts = { 0, 20 * 1000 * 1000 }; /* 20ms */
            nanosleep(&ts, NULL);
        }
        (void)reaped;
    }

    if (WIFEXITED(st)) {
        out._0 = WEXITSTATUS(st);
    } else if (WIFSIGNALED(st)) {
        out._0 = 128 + WTERMSIG(st);
    } else {
        out._0 = -1;
        out._2 = "child terminated abnormally";
    }
    return out;
}

/* Signal-forwarding state for os_run_supervised_raw. A supervised run
 * installs SIGINT/SIGTERM handlers that forward the signal to the
 * child's process group, so a Ctrl-C at the controlling terminal tears
 * down the whole build rather than orphaning the parent's children.
 * Single-slot global: v1 supports one supervised run forwarding at a
 * time (nested/concurrent supervised runs in threads would race this).
 * The handler does nothing but kill(2), which is async-signal-safe. */
static volatile sig_atomic_t g_supervised_pgid = 0;

static void aether_os_forward_signal(int sig) {
    pid_t pg = (pid_t)g_supervised_pgid;
    if (pg > 0) kill(-pg, sig);
}

/* One-call supervised run. Encodes the bash job-control pattern that
 * every build/test orchestrator reinvents:
 *
 *   - new_process_group: run the child in its own process group
 *     (pgid == child pid) so signals and reaping can target the whole
 *     subtree, not just the immediate child.
 *   - forward_signals: install parent SIGINT/SIGTERM handlers that
 *     forward to the child's group (requires new_process_group).
 *   - timeout_secs > 0: TERM the group on deadline, grace 5s, then
 *     KILL; report exit_code 124 (GNU `timeout` convention).
 *   - reap_group: after the child exits, TERM→grace→KILL the group to
 *     clean up anything the build leaked (a stray test server, a
 *     backgrounded helper), then reap the now-dead members (requires
 *     new_process_group).
 *
 * Returns (exit_code, outcome) where outcome is one of:
 *   "exited"    — child exited normally; exit_code is its status.
 *   "signalled" — child was killed by a signal; exit_code is 128+signo.
 *   "timeout"   — deadline hit, group torn down; exit_code is 124.
 *   "error"     — spawn / wait failure; exit_code is -1.
 */
_tuple_int_string os_run_supervised_raw(const char* prog, void* argv_list, void* env_list,
                                        int new_process_group, int forward_signals,
                                        int timeout_secs, int reap_group) {
    _tuple_int_string out = { -1, "error" };
    if (!prog) { out._1 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._1 = "denied by sandbox"; return out; }

    char** av = build_argv_array(prog, argv_list);
    if (!av) { out._1 = "argv build failed"; return out; }
    char** envp = build_envp_array(env_list);

    pid_t pid = fork();
    if (pid < 0) {
        free(av); free(envp);
        out._1 = "fork failed";
        return out;
    }
    if (pid == 0) {
        /* Child: become our own process-group leader before exec so the
         * parent can address the whole subtree as -pid. Setting it in
         * both child and parent (below) closes the fork/exec race. */
        if (new_process_group) setpgid(0, 0);
        if (envp) execve(prog, av, envp);
        else      execvp(prog, av);
        _exit(127);
    }
    /* Parent. */
    if (new_process_group) setpgid(pid, pid); /* harmless if child won the race */
    free(av); free(envp);

    /* The group we can address with a negative pid. 0 means "no group"
     * (new_process_group was off) — signal/reap-group steps are skipped. */
    pid_t group = new_process_group ? pid : 0;

    struct sigaction old_int, old_term, sa;
    int installed = 0;
    if (forward_signals && group) {
        g_supervised_pgid = pid;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = aether_os_forward_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; /* no SA_RESTART: let waitpid return EINTR so we re-loop */
        sigaction(SIGINT, &sa, &old_int);
        sigaction(SIGTERM, &sa, &old_term);
        installed = 1;
    }

    int st = 0;
    const char* outcome = "exited";
    int exit_code = -1;
    int handled = 0; /* 1 once outcome/exit_code are final (timeout path) */

    if (timeout_secs > 0) {
        int64_t deadline = os_now_monotonic_ms_raw() + (int64_t)timeout_secs * 1000;
        int reaped = 0;
        for (;;) {
            pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid) { reaped = 1; break; }
            if (r < 0) {
                if (errno == EINTR) continue;
                break; /* ECHILD or similar — fall through to status decode */
            }
            if (os_now_monotonic_ms_raw() >= deadline) break;
            struct timespec ts = { 0, 20 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        if (!reaped) {
            /* Deadline hit: TERM the group (or the lone child), give it
             * 5s to unwind, then KILL and definitely reap. */
            pid_t target = group ? -group : pid;
            kill(target, SIGTERM);
            int64_t grace = os_now_monotonic_ms_raw() + 5000;
            while (os_now_monotonic_ms_raw() < grace) {
                if (waitpid(pid, &st, WNOHANG) == pid) { reaped = 1; break; }
                struct timespec ts = { 0, 20 * 1000 * 1000 };
                nanosleep(&ts, NULL);
            }
            if (!reaped) {
                kill(target, SIGKILL);
                waitpid(pid, &st, 0);
            }
            outcome = "timeout";
            exit_code = 124;
            handled = 1;
        }
    } else {
        while (waitpid(pid, &st, 0) < 0) {
            if (errno != EINTR) { outcome = "error"; exit_code = -1; handled = 1; break; }
        }
    }

    if (!handled) {
        if (WIFEXITED(st)) {
            exit_code = WEXITSTATUS(st);
            outcome = "exited";
        } else if (WIFSIGNALED(st)) {
            exit_code = 128 + WTERMSIG(st);
            outcome = "signalled";
        } else {
            exit_code = -1;
            outcome = "error";
        }
    }

    /* Group-reap anything the build leaked into the group. The child
     * itself is already reaped; these waitpid(-group) calls collect any
     * other group members we just signalled so they don't linger as
     * zombies. */
    if (reap_group && group) {
        kill(-group, SIGTERM);
        struct timespec ts = { 0, 100 * 1000 * 1000 }; /* 100ms grace */
        nanosleep(&ts, NULL);
        kill(-group, SIGKILL);
        for (;;) {
            int st2 = 0;
            pid_t r = waitpid(-group, &st2, WNOHANG);
            if (r <= 0) break;
        }
    }

    if (installed) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGTERM, &old_term, NULL);
        g_supervised_pgid = 0;
    }

    out._0 = exit_code;
    out._1 = outcome;
    return out;
}

typedef struct { const char* _0; const char* _1; int _2; const char* _3; } _tuple_string_string_int_string;

/* Append `n` bytes to a doubling buffer, keeping room for a trailing
 * NUL. Returns 1 on success, 0 on allocation failure (buffer left
 * intact for the caller to free). Used by os_run_full_raw's poll
 * loop to accumulate stdout and stderr independently. */
static int aether_os_buf_append(char** buf, size_t* len, size_t* cap,
                                const char* data, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 4096;
        while (*len + n + 1 > nc) nc *= 2;
        /* #462: cap the captured stream (caller os_run_full_raw bounds
         * stdout/stderr through this). realloc(NULL, ...) on the first
         * grow behaves as malloc; the old size is *cap (0 initially). */
        char* bigger = (char*)aether_caps_realloc(*buf, *cap, nc);
        if (!bigger) return 0;
        *buf = bigger;
        *cap = nc;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    return 1;
}

static void aether_os_set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Full three-way child stdio: feed `stdin_data` (exactly `stdin_len`
 * bytes — binary-safe, NUL bytes included) to the child's stdin, and
 * capture its stdout and stderr SEPARATELY. Returns
 * (stdout, stderr, exit_code, err):
 *   - stdout / stderr: @heap captures the caller owns; "" if empty.
 *     Like os.run_capture they are NUL-terminated C strings, so a
 *     capture containing an embedded NUL reads as truncated through
 *     the string wrappers (the stdin direction has no such limit —
 *     it is written by explicit length).
 *   - exit_code: WEXITSTATUS on normal exit, 128+signo if the child
 *     was killed by a signal, 127 on exec failure, -1 on spawn/IO
 *     error (err non-empty in that last case).
 *   - err: "" on a successful spawn (regardless of exit code);
 *     non-empty only when the fork/exec/pipe couldn't run.
 *
 * A poll(2) loop pumps all three fds at once, so the child can write
 * unbounded stdout/stderr while we stream its stdin — no classic
 * "fill the pipe buffer and deadlock" hazard. `stdin_len <= 0` gives
 * the child an immediately-closed (empty) stdin rather than the
 * parent's, so it can't block reading a terminal. */
_tuple_string_string_int_string os_run_full_raw(const char* prog, void* argv_list, void* env_list,
                                                const char* stdin_data, int stdin_len) {
    _tuple_string_string_int_string out = {
        aether_os_empty_heap(), aether_os_empty_heap(), -1, "" };
    if (!prog) { out._3 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._3 = "denied by sandbox"; return out; }

    int in_pipe[2]  = { -1, -1 };
    int out_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    if (pipe(in_pipe) != 0) { out._3 = "pipe failed"; return out; }
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        out._3 = "pipe failed"; return out;
    }
    if (pipe(err_pipe) != 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        out._3 = "pipe failed"; return out;
    }

    char** av = build_argv_array(prog, argv_list);
    if (!av) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        out._3 = "argv build failed"; return out;
    }
    char** envp = build_envp_array(env_list);

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        free(av); free(envp);
        out._3 = "fork failed"; return out;
    }
    if (pid == 0) {
        /* Child: wire stdin/stdout/stderr to the pipes, close the rest. */
        close(in_pipe[1]); close(out_pipe[0]); close(err_pipe[0]);
        if (dup2(in_pipe[0], 0) < 0)  _exit(127);
        if (dup2(out_pipe[1], 1) < 0) _exit(127);
        if (dup2(err_pipe[1], 2) < 0) _exit(127);
        close(in_pipe[0]); close(out_pipe[1]); close(err_pipe[1]);
        if (envp) execve(prog, av, envp);
        else      execvp(prog, av);
        _exit(127);
    }
    /* Parent. */
    free(av); free(envp);
    close(in_pipe[0]); close(out_pipe[1]); close(err_pipe[1]);
    int in_fd = in_pipe[1], o_fd = out_pipe[0], e_fd = err_pipe[0];

    size_t in_total = (stdin_data && stdin_len > 0) ? (size_t)stdin_len : 0;
    size_t in_off = 0;
    int in_open = 1;
    if (in_total == 0) { close(in_fd); in_open = 0; } /* empty stdin → immediate EOF */

    aether_os_set_nonblock(o_fd);
    aether_os_set_nonblock(e_fd);
    if (in_open) aether_os_set_nonblock(in_fd);

    char* obuf = NULL; size_t olen = 0, ocap = 0;
    char* ebuf = NULL; size_t elen = 0, ecap = 0;
    int o_open = 1, e_open = 1, oom = 0;
    char tmp[4096];

    while (o_open || e_open || in_open) {
        struct pollfd pfds[3];
        int nf = 0, oi = -1, ei = -1, ii = -1;
        if (o_open)  { pfds[nf].fd = o_fd;  pfds[nf].events = POLLIN;  pfds[nf].revents = 0; oi = nf++; }
        if (e_open)  { pfds[nf].fd = e_fd;  pfds[nf].events = POLLIN;  pfds[nf].revents = 0; ei = nf++; }
        if (in_open) { pfds[nf].fd = in_fd; pfds[nf].events = POLLOUT; pfds[nf].revents = 0; ii = nf++; }

        if (poll(pfds, nf, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (oi >= 0 && pfds[oi].revents) {
            for (;;) {
                ssize_t n = read(o_fd, tmp, sizeof tmp);
                if (n > 0) { if (!aether_os_buf_append(&obuf, &olen, &ocap, tmp, (size_t)n)) { oom = 1; o_open = 0; break; } continue; }
                if (n == 0) { o_open = 0; break; }
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) { if (pfds[oi].revents & (POLLHUP | POLLERR)) o_open = 0; break; }
                o_open = 0; break;
            }
        }
        if (ei >= 0 && pfds[ei].revents) {
            for (;;) {
                ssize_t n = read(e_fd, tmp, sizeof tmp);
                if (n > 0) { if (!aether_os_buf_append(&ebuf, &elen, &ecap, tmp, (size_t)n)) { oom = 1; e_open = 0; break; } continue; }
                if (n == 0) { e_open = 0; break; }
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) { if (pfds[ei].revents & (POLLHUP | POLLERR)) e_open = 0; break; }
                e_open = 0; break;
            }
        }
        if (ii >= 0 && pfds[ii].revents) {
            size_t remain = in_total - in_off;
            size_t chunk = remain > 65536 ? 65536 : remain;
            ssize_t w = write(in_fd, stdin_data + in_off, chunk);
            if (w > 0) {
                in_off += (size_t)w;
                if (in_off >= in_total) { close(in_fd); in_open = 0; }
            } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                /* EPIPE etc — child closed stdin early. Stop feeding. */
                close(in_fd); in_open = 0;
            }
            if (oom) break;
        }
        if (oom) break;
    }

    close(o_fd);
    close(e_fd);
    if (in_open) close(in_fd);

    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) {
            /* #462: free the capped capture buffers with their live
             * sizes (caps_free(NULL,0) is a no-op when never grown). */
            aether_caps_free(obuf, ocap); aether_caps_free(ebuf, ecap);
            out._2 = -1;
            out._3 = "waitpid failed";
            return out;
        }
    }

    if (oom) {
        aether_caps_free(obuf, ocap); aether_caps_free(ebuf, ecap);
        out._2 = -1;
        out._3 = "alloc failed";
        return out;
    }

    /* NUL-terminate the captures and hand ownership to the caller,
     * freeing the empty-heap placeholders first (same #420 v2 contract
     * the other @heap returns follow). */
    if (obuf) { obuf[olen] = '\0'; free((void*)out._0); out._0 = obuf; }
    if (ebuf) { ebuf[elen] = '\0'; free((void*)out._1); out._1 = ebuf; }

    if (WIFEXITED(st))        out._2 = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) out._2 = 128 + WTERMSIG(st);
    else                      out._2 = -1;
    return out;
}

/* Child side: returns the parent-channel fd if the parent spawned
 * us via os_run_pipe* (i.e. AETHER_IPC_FD is set and the named fd
 * is open writable); -1 otherwise.
 *
 * Implemented in std/ipc/aether_ipc.c — declared here only so the
 * Aether module.ae's `extern ipc_parent_channel_raw()` resolves
 * during link. */

#else // _WIN32

// -----------------------------------------------------------------
// Windows process-exec backend: CreateProcessW with argv-style launch.
//
// The POSIX branch uses execvp directly. On Windows, CreateProcessW
// takes a single UTF-16 command-line string with the very particular
// escaping rules consumed by the C runtime's argv parser. The helpers
// below build that string correctly — the classic "Everyone quotes
// command line arguments the wrong way" rules are applied verbatim.
// -----------------------------------------------------------------

// UTF-8 → UTF-16. Caller frees. Returns NULL on conversion failure.
static wchar_t* utf8_to_wide(const char* utf8) {
    if (!utf8) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t* wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)wlen);
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, wlen) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

// UTF-16 → UTF-8. Caller frees. Returns NULL on conversion failure.
static char* wide_to_utf8(const wchar_t* wide) {
    if (!wide) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char* utf8 = (char*)malloc((size_t)len);
    if (!utf8) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, len, NULL, NULL) <= 0) {
        free(utf8);
        return NULL;
    }
    return utf8;
}

// Simple growing wide-string buffer for assembling a command line.
typedef struct {
    wchar_t* data;
    size_t   len;
    size_t   cap;
    int      oom;
} WBuf;

static void wbuf_init(WBuf* b) { b->data = NULL; b->len = 0; b->cap = 0; b->oom = 0; }

static int wbuf_reserve(WBuf* b, size_t extra) {
    if (b->oom) return 0;
    size_t need = b->len + extra + 1;
    if (need > b->cap) {
        size_t new_cap = aether_buf_grow_capacity(b->cap, 256, need, sizeof(wchar_t));
        if (!new_cap) { b->oom = 1; return 0; }
        wchar_t* nd = (wchar_t*)realloc(b->data, sizeof(wchar_t) * new_cap);
        if (!nd) { b->oom = 1; return 0; }
        b->data = nd;
        b->cap = new_cap;
    }
    return 1;
}

static void wbuf_append(WBuf* b, const wchar_t* s) {
    if (!s) return;
    size_t slen = wcslen(s);
    if (!wbuf_reserve(b, slen)) return;
    memcpy(b->data + b->len, s, sizeof(wchar_t) * slen);
    b->len += slen;
    b->data[b->len] = L'\0';
}

static void wbuf_append_char(WBuf* b, wchar_t c) {
    if (!wbuf_reserve(b, 1)) return;
    b->data[b->len++] = c;
    b->data[b->len] = L'\0';
}

// Escape and append `arg` for CRT argv parsing. Rules from MSFT:
// https://learn.microsoft.com/en-us/archive/blogs/twistylittlepassagesallalike/everyone-quotes-command-line-arguments-the-wrong-way
//   - If the arg contains no whitespace and no '"', append verbatim.
//   - Otherwise wrap in quotes. Inside the quotes:
//       * Any run of N backslashes followed by a '"' becomes 2N+1
//         backslashes followed by \".
//       * Any run of N backslashes at the very end of the arg (right
//         before the closing quote) becomes 2N backslashes.
//       * Other backslashes are passed through verbatim.
static void append_escaped_arg(WBuf* b, const wchar_t* arg) {
    int needs_quote = 0;
    if (*arg == L'\0') {
        needs_quote = 1;
    } else {
        for (const wchar_t* p = arg; *p; p++) {
            if (*p == L' ' || *p == L'\t' || *p == L'"') {
                needs_quote = 1;
                break;
            }
        }
    }

    if (!needs_quote) {
        wbuf_append(b, arg);
        return;
    }

    wbuf_append_char(b, L'"');

    const wchar_t* p = arg;
    while (*p) {
        size_t backslashes = 0;
        while (*p == L'\\') { backslashes++; p++; }

        if (*p == L'\0') {
            // Trailing backslashes before closing quote — double them.
            for (size_t i = 0; i < backslashes * 2; i++) wbuf_append_char(b, L'\\');
            break;
        } else if (*p == L'"') {
            // N backslashes + '"' → 2N+1 backslashes + \"
            for (size_t i = 0; i < backslashes * 2 + 1; i++) wbuf_append_char(b, L'\\');
            wbuf_append_char(b, L'"');
            p++;
        } else {
            for (size_t i = 0; i < backslashes; i++) wbuf_append_char(b, L'\\');
            wbuf_append_char(b, *p);
            p++;
        }
    }

    wbuf_append_char(b, L'"');
}

// Build the command-line string for CreateProcessW. argv[0] is the
// program name — we use `prog` if the caller's list is empty, else the
// first list entry. Caller frees.
static wchar_t* build_command_line(const char* prog, void* argv_list) {
    WBuf b; wbuf_init(&b);

    /* Match the POSIX argv convention used by build_argv_array: the program
     * is always prog, and argv_list holds the caller's arguments starting at
     * position 0. The earlier shape — treating argv_list[0] as the program
     * when the list was non-empty — meant os.run("cmd", ["/c","echo","x"])
     * produced the command line `/c echo x` and tried to spawn a program
     * named `/c`. Discovered chasing the second half of issue #706 once
     * lpApplicationName=NULL exposed cmdline[0] to CreateProcessW's parser. */
    wchar_t* wprog = utf8_to_wide(prog);
    if (!wprog) { free(b.data); return NULL; }
    append_escaped_arg(&b, wprog);
    free(wprog);

    int n = argv_list ? list_size(argv_list) : 0;
    for (int i = 0; i < n; i++) {
        const char* item = aether_string_data(list_get_raw(argv_list, i));
        if (!item) continue;
        wchar_t* w = utf8_to_wide(item);
        if (!w) { free(b.data); return NULL; }
        wbuf_append_char(&b, L' ');
        append_escaped_arg(&b, w);
        free(w);
    }

    if (b.oom) { free(b.data); return NULL; }
    return b.data;
}

// Build a UTF-16 environment block from a list of "KEY=VALUE" strings.
// Format: key1=val1\0key2=val2\0\0. Returns NULL if env_list is NULL
// (inherit parent env) or on allocation failure.
static wchar_t* build_environ_block(void* env_list) {
    if (!env_list) return NULL;
    int n = list_size(env_list);
    WBuf b; wbuf_init(&b);

    for (int i = 0; i < n; i++) {
        const char* item = aether_string_data(list_get_raw(env_list, i));
        if (!item) continue;
        wchar_t* w = utf8_to_wide(item);
        if (!w) { free(b.data); return NULL; }
        wbuf_append(&b, w);
        wbuf_append_char(&b, L'\0');
        free(w);
    }
    wbuf_append_char(&b, L'\0');
    if (b.oom) { free(b.data); return NULL; }
    return b.data;
}

// Shared launch path. If `capture_stdout` is non-zero we redirect the
// child's stdout to a pipe and read it to completion. `out_exit_code`
// and `out_capture` are optional outputs.
static int win_launch(const char* prog, void* argv_list, void* env_list,
                      int capture_stdout,
                      int* out_exit_code, char** out_capture) {
    /* prog seeds build_command_line's argv[0] (matching POSIX
     * build_argv_array). CreateProcessW receives lpApplicationName=NULL
     * and PATH-resolves that first token itself (see below). */
    wchar_t* cmdline = build_command_line(prog, argv_list);
    if (!cmdline) return -1;

    wchar_t* wenv = build_environ_block(env_list);
    // NULL from build_environ_block when env_list is NULL means "inherit" —
    // that's the correct semantic, not an error.

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);

    HANDLE read_pipe = NULL, write_pipe = NULL;
    SECURITY_ATTRIBUTES sa;

    if (capture_stdout) {
        memset(&sa, 0, sizeof(sa));
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = NULL;
        if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
            free(cmdline); free(wenv);
            return -1;
        }
        // The read end must NOT be inherited by the child.
        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = write_pipe;
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    }

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    /* lpApplicationName=NULL so CreateProcessW parses the first whitespace-
     * delimited token of lpCommandLine and runs its own PATH + .exe + .bat
     * resolution. Passing a bare program name (e.g. "cmd", "gcc") as
     * lpApplicationName makes Win32 treat it as a literal filename in CWD
     * — no PATH search, no extension append — and CreateProcessW returns
     * ERROR_FILE_NOT_FOUND. That broke os.run / os.run_capture /
     * os.run_supervised on Windows for every PATH-resolved program
     * (issue #706). The argv[0] in cmdline (built by build_command_line)
     * already carries the program name, properly quoted by append_escaped_arg,
     * which is what CreateProcessW will parse. */
    BOOL ok = CreateProcessW(
        NULL,          // application name — let CreateProcessW PATH-resolve cmdline[0]
        cmdline,       // command line (modifiable — CreateProcessW may write)
        NULL, NULL,
        capture_stdout ? TRUE : FALSE,  // inherit handles only when capturing
        flags,
        wenv,          // NULL = inherit parent env
        NULL,          // CWD = current
        &si,
        &pi);

    free(cmdline);
    free(wenv);

    if (capture_stdout) {
        // Parent closes the write end so EOF happens when the child exits.
        CloseHandle(write_pipe);
    }

    if (!ok) {
        if (capture_stdout) CloseHandle(read_pipe);
        return -1;
    }

    char* capture_buf = NULL;
    size_t capture_len = 0;

    if (capture_stdout) {
        size_t cap = 1024;
        capture_buf = (char*)malloc(cap);
        if (!capture_buf) {
            CloseHandle(read_pipe);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            return -1;
        }
        char tmp[1024];
        DWORD got = 0;
        while (ReadFile(read_pipe, tmp, sizeof(tmp), &got, NULL) && got > 0) {
            if (capture_len + got + 1 > cap) {
                while (capture_len + got + 1 > cap) cap *= 2;
                char* bigger = (char*)realloc(capture_buf, cap);
                if (!bigger) {
                    free(capture_buf);
                    CloseHandle(read_pipe);
                    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                    return -1;
                }
                capture_buf = bigger;
            }
            memcpy(capture_buf + capture_len, tmp, got);
            capture_len += got;
        }
        capture_buf[capture_len] = '\0';
        CloseHandle(read_pipe);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (out_exit_code) *out_exit_code = (int)exit_code;
    if (out_capture) *out_capture = capture_buf;
    else free(capture_buf);

    return 0;
}

/* ============================================================
 * Non-blocking spawn + reap on Windows (no IPC pipe).
 *
 * win_spawn is win_launch's front half: build the command line (reusing
 * append_escaped_arg's CreateProcessW quoting), CreateProcessW, return the
 * live process HANDLE without waiting. The wait/exit-code/close half lives
 * in os_wait_raw / os_wait_any_raw.
 *
 * The Aether "token" ABI is int, but a HANDLE is a pointer — and a raw
 * dwProcessId can be recycled after exit, so reaping by PID could race onto
 * a different process. We therefore hand out a small monotonically-growing
 * int token and keep the live HANDLE in a table keyed by that token. The
 * token is never recycled within a run, so a stale token cleanly misses. */

typedef struct { int token; HANDLE h; } WinProc;
static WinProc  g_winprocs[4096];
static int      g_winproc_count = 0;
static int      g_winproc_next_token = 1;
static CRITICAL_SECTION g_winproc_cs;
static int      g_winproc_cs_init = 0;

static void winproc_lock(void) {
    /* One-time CS init. Spawns are effectively single-threaded from a build
     * scheduler, but guard anyway so a threaded caller can't corrupt the
     * table. The init race is itself benign for the single-threaded case. */
    if (!g_winproc_cs_init) { InitializeCriticalSection(&g_winproc_cs); g_winproc_cs_init = 1; }
    EnterCriticalSection(&g_winproc_cs);
}
static void winproc_unlock(void) { LeaveCriticalSection(&g_winproc_cs); }

/* Register a live handle, return its token (or -1 if the table is full). */
static int winproc_register(HANDLE h) {
    winproc_lock();
    if (g_winproc_count >= (int)(sizeof(g_winprocs)/sizeof(g_winprocs[0]))) {
        winproc_unlock();
        return -1;
    }
    int tok = g_winproc_next_token++;
    g_winprocs[g_winproc_count].token = tok;
    g_winprocs[g_winproc_count].h = h;
    g_winproc_count++;
    winproc_unlock();
    return tok;
}

/* Look up a token's handle without removing it. Returns NULL if absent. */
static HANDLE winproc_find(int token) {
    HANDLE h = NULL;
    winproc_lock();
    for (int i = 0; i < g_winproc_count; i++) {
        if (g_winprocs[i].token == token) { h = g_winprocs[i].h; break; }
    }
    winproc_unlock();
    return h;
}

/* Remove a token's slot (after it has been reaped + CloseHandle'd). */
static void winproc_remove(int token) {
    winproc_lock();
    for (int i = 0; i < g_winproc_count; i++) {
        if (g_winprocs[i].token == token) {
            g_winprocs[i] = g_winprocs[g_winproc_count - 1];
            g_winproc_count--;
            break;
        }
    }
    winproc_unlock();
}

/* Front half of win_launch: spawn without waiting. Returns the process
 * HANDLE, or NULL on failure. hThread is closed here (never needed by the
 * caller); hProcess is the caller's to wait on and close. */
static HANDLE win_spawn(const char* prog, void* argv_list, void* env_list) {
    wchar_t* cmdline = build_command_line(prog, argv_list);
    if (!cmdline) return NULL;
    wchar_t* wenv = build_environ_block(env_list);

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    /* Same CreateProcessW contract as win_launch: lpApplicationName=NULL so
     * cmdline[0] is PATH-resolved (issue #706); no handle inheritance (no
     * pipe), inherit parent env when wenv is NULL. */
    BOOL ok = CreateProcessW(
        NULL, cmdline, NULL, NULL, FALSE,
        CREATE_UNICODE_ENVIRONMENT, wenv, NULL, &si, &pi);

    free(cmdline);
    free(wenv);

    if (!ok) return NULL;
    CloseHandle(pi.hThread);   /* thread handle never used */
    return pi.hProcess;
}

/* Working-directory accessors — Windows. UTF-8 path in / UTF-8 path out,
 * converted through the wide-char helpers so non-ASCII paths survive. */
int os_chdir_raw(const char* path) {
    if (!path) return -1;
    if (!aether_sandbox_check("fs", path)) return -1;
    wchar_t* w = utf8_to_wide(path);
    if (!w) return -1;
    BOOL ok = SetCurrentDirectoryW(w);
    free(w);
    return ok ? 0 : -1;
}

char* os_getcwd_raw(void) {
    /* GetCurrentDirectoryW(0, NULL) returns the buffer size (incl. NUL). */
    DWORD need = GetCurrentDirectoryW(0, NULL);
    if (need == 0) return NULL;
    /* #462: transient wide-char cwd scratch — alloc'd and freed locally,
     * sized by the (plugin-influenced) path length. The returned UTF-8
     * `utf8` stays raw (caller-owned). */
    size_t w_bytes = sizeof(wchar_t) * (size_t)need;
    wchar_t* w = (wchar_t*)aether_caps_malloc(w_bytes);
    if (!w) return NULL;
    DWORD got = GetCurrentDirectoryW(need, w);
    if (got == 0 || got >= need) { aether_caps_free(w, w_bytes); return NULL; }
    char* utf8 = wide_to_utf8(w);
    aether_caps_free(w, w_bytes);
    return utf8;
}

int os_run(const char* prog, void* argv_list, void* env_list) {
    if (!prog) return -1;
    if (!aether_sandbox_check("exec", prog)) return -1;

    int exit_code = 0;
    if (win_launch(prog, argv_list, env_list, 0, &exit_code, NULL) != 0) {
        return -1;
    }
    return exit_code;
}

char* os_run_capture_raw(const char* prog, void* argv_list, void* env_list) {
    if (!prog) return NULL;
    if (!aether_sandbox_check("exec", prog)) return NULL;

    char* capture = NULL;
    int exit_code = 0;
    if (win_launch(prog, argv_list, env_list, 1, &exit_code, &capture) != 0) {
        free(capture);
        return NULL;
    }
    // Caller conventions: on non-zero exit the POSIX branch still
    // returns the captured stdout so the caller can inspect it.
    (void)exit_code;
    return capture ? capture : strdup("");
}

/* Tuple sibling — exposes exit status. See the POSIX branch for the
 * full contract. Issue #289. */
typedef struct { const char* _0; int _1; const char* _2; } _tuple_string_int_string;

_tuple_string_int_string os_run_capture_status_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "" };
    if (!prog) { out._2 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._2 = "denied by sandbox"; return out; }

    char* capture = NULL;
    int exit_code = 0;
    if (win_launch(prog, argv_list, env_list, 1, &exit_code, &capture) != 0) {
        free(capture);
        out._2 = "spawn failed";
        return out;
    }
    /* Free the empty-heap initializer before overwriting, so the
     * success path doesn't leak a 1-byte allocation per call. */
    free((void*)out._0);
    out._0 = capture ? capture : aether_os_empty_heap();
    out._1 = exit_code;
    /* err stays "" — Win32 win_launch already returned 0 here, so
     * the spawn itself succeeded. */
    return out;
}

/* std.ipc on Windows: stubs returning unsupported.
 *
 * Windows handle inheritance via STARTUPINFOEX +
 * PROC_THREAD_ATTRIBUTE_HANDLE_LIST is the right approach for a
 * future port, but mapping "child sees this at fd 3" requires
 * coordinated _open_osfhandle on both sides plus careful handling
 * of which std handles get inherited. v1 ships POSIX-only;
 * Windows consumers fall back to the file-marker pattern. */

typedef struct { int _0; int _1; const char* _2; } _tuple_int_int_string;
typedef struct { int _0; const char* _1; } _tuple_int_string;

/* Reap one token: wait on its handle, read exit code, close, free slot.
 * Shared by os_wait_raw and (per-handle) os_wait_any_raw. */
static _tuple_int_string winproc_reap(int token, HANDLE h) {
    _tuple_int_string out = { -1, "" };
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    winproc_remove(token);
    out._0 = (int)code;
    return out;
}

/* Non-blocking spawn (no pipe). Returns (token, err). */
_tuple_int_string os_spawn_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_int_string out = { -1, "" };
    if (!prog) { out._1 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._1 = "denied by sandbox"; return out; }
    HANDLE h = win_spawn(prog, argv_list, env_list);
    if (!h) { out._1 = "spawn failed"; return out; }
    int tok = winproc_register(h);
    if (tok < 0) {
        /* Table full — don't leak the process: wait+close it and error. */
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        out._1 = "process table full";
        return out;
    }
    out._0 = tok;
    return out;
}

/* Reap a single token. Returns (exit, err). */
_tuple_int_string os_wait_raw(int token) {
    _tuple_int_string out = { -1, "" };
    if (token <= 0) { out._1 = "invalid token"; return out; }
    HANDLE h = winproc_find(token);
    if (!h) { out._1 = "unknown token"; return out; }
    return winproc_reap(token, h);
}

/* Reap whichever token finishes first. Returns (token, exit, err).
 * WaitForMultipleObjects gives a true wait-any over the live handles. */
_tuple_int_int_string os_wait_any_raw(void* token_list) {
    _tuple_int_int_string out = { -1, -1, "" };
    int n = token_list ? list_size(token_list) : 0;
    if (n <= 0) { out._2 = "no tokens"; return out; }
    if (n > MAXIMUM_WAIT_OBJECTS) { out._2 = "too many tokens for one wait_any"; return out; }

    /* Snapshot (token, handle) pairs for the live tokens the caller named.
     * Tokens are small ints stored INLINE as the list's pointer slot (the
     * caller boxes them with mem.long_to_ptr(token)); read them back as the
     * integer value of the pointer, NOT as a pointer-to-int. Only tokens
     * still in the table (not yet reaped) contribute a handle. */
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    int    toks[MAXIMUM_WAIT_OBJECTS];
    int    m = 0;
    for (int i = 0; i < n; i++) {
        void* item = list_get_raw(token_list, i);
        int tok = (int)(intptr_t)item;
        HANDLE h = winproc_find(tok);
        if (h) { handles[m] = h; toks[m] = tok; m++; }
    }
    if (m == 0) { out._2 = "no live tokens"; return out; }

    DWORD r = WaitForMultipleObjects((DWORD)m, handles, FALSE, INFINITE);
    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + (DWORD)m) {
        int idx = (int)(r - WAIT_OBJECT_0);
        _tuple_int_string s = winproc_reap(toks[idx], handles[idx]);
        out._0 = toks[idx];
        out._1 = s._0;
        out._2 = s._1;
        return out;
    }
    out._2 = "wait_any failed";
    return out;
}

/* run_pipe on Windows: the SPAWN works (no back-channel pipe — that half
 * needs coordinated _open_osfhandle and stays POSIX-only). Returns a -1
 * pipe fd, the token, and ""; reap the token via os_wait_pid_raw. */
_tuple_int_int_string os_run_pipe_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_int_int_string out = { -1, -1, "" };
    _tuple_int_string sp = os_spawn_raw(prog, argv_list, env_list);
    if (sp._1[0]) { out._2 = sp._1; return out; }   /* err */
    out._0 = -1;        /* no back-channel pipe on Windows */
    out._1 = sp._0;     /* token doubles as the wait_pid handle */
    return out;
}

_tuple_int_string os_wait_pid_raw(int pid) {
    /* pid here is the token minted by os_run_pipe_raw / os_spawn_raw. */
    return os_wait_raw(pid);
}

_tuple_string_int_string os_run_pipe_drain_and_wait_raw(const char* prog, void* argv_list, void* env_list) {
    /* The back-channel pipe is the POSIX-only part; no Windows equivalent. */
    (void)prog; (void)argv_list; (void)env_list;
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "unsupported on Windows" };
    return out;
}

/* Process-supervision primitives on Windows.
 *
 * POSIX process groups + killpg + sigaction don't exist here; the clean
 * Win32 mapping is:
 *   - process *group* lifecycle (timeout, group-reap of leaked children)
 *     → a Job Object. JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE makes closing
 *     the job tear down the whole tree, which IS group-reap;
 *     TerminateJobObject is the group kill;
 *   - signal forwarding → a console control handler that terminates the
 *     job on Ctrl-C / Ctrl-Break (console apps only; there are no
 *     catchable POSIX signals to forward);
 *   - waiting / timeout → WaitForSingleObject on the process handle.
 * There is no "killed by signal" outcome on Windows, so run_supervised
 * reports "exited" (or "timeout"); os.kill maps any non-zero signal to a
 * forced TerminateProcess (no graceful SIGTERM) and rejects negative-pid
 * group signalling (no pgid-by-negative-pid concept). */

int os_kill_raw(int pid, int sig) {
    if (!aether_sandbox_check("exec", "kill")) return -1;
    /* Negative pid = signal a whole group: no Win32 equivalent. */
    if (pid < 0) return -1;
    if (sig == 0) {
        /* Existence probe: open + a zero-timeout wait. WAIT_TIMEOUT means
         * the process is still running (alive); WAIT_OBJECT_0 means it has
         * already exited (gone) — matches POSIX kill -0 semantics. */
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
        if (!h) return -1;
        DWORD w = WaitForSingleObject(h, 0);
        CloseHandle(h);
        return (w == WAIT_TIMEOUT) ? 0 : -1;
    }
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!h) return -1;
    /* Exit code mirrors POSIX 128+signo so a waiter can tell it was a kill. */
    BOOL ok = TerminateProcess(h, (UINT)(128 + sig));
    CloseHandle(h);
    return ok ? 0 : -1;
}

_tuple_int_int_string os_wait_pid_timeout_raw(int pid, int secs) {
    _tuple_int_int_string out = { -1, 0, "" };
    if (pid <= 0) { out._2 = "invalid pid"; return out; }
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);
    if (!h) { out._2 = "no such process"; return out; }
    DWORD ms = (secs <= 0) ? INFINITE : (DWORD)secs * 1000;
    DWORD w = WaitForSingleObject(h, ms);
    if (w == WAIT_TIMEOUT) { out._1 = 1; CloseHandle(h); return out; }
    if (w != WAIT_OBJECT_0) { out._2 = "wait failed"; CloseHandle(h); return out; }
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    out._0 = (int)code;
    return out;
}

/* Forward-signals state: the job to tear down on Ctrl-C / Ctrl-Break.
 * Single supervised run at a time (mirrors the POSIX g_supervised_pgid). */
static HANDLE g_supervised_job = NULL;

static BOOL WINAPI aether_os_ctrl_handler(DWORD ctrl) {
    if ((ctrl == CTRL_C_EVENT || ctrl == CTRL_BREAK_EVENT) && g_supervised_job) {
        TerminateJobObject(g_supervised_job, (UINT)(128 + 2)); /* SIGINT-ish */
        return TRUE;
    }
    return FALSE;
}

_tuple_int_string os_run_supervised_raw(const char* prog, void* argv_list, void* env_list,
                                        int new_process_group, int forward_signals,
                                        int timeout_secs, int reap_group) {
    _tuple_int_string out = { -1, "error" };
    if (!prog) { out._1 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._1 = "denied by sandbox"; return out; }

    wchar_t* cmdline = build_command_line(prog, argv_list);
    if (!cmdline) { out._1 = "argv build failed"; return out; }
    wchar_t* wenv = build_environ_block(env_list); /* NULL = inherit */

    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) { free(cmdline); free(wenv); out._1 = "job create failed"; return out; }

    /* Arm kill-on-close only when reap_group is set: closing the job then
     * tears down the whole tree (the Windows group-reap). Without it,
     * leaked children outlive the job-handle close. */
    if (reap_group) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        memset(&jeli, 0, sizeof jeli);
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof jeli);
    }

    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);

    /* CREATE_SUSPENDED so we can assign to the job before the child runs —
     * otherwise a fast child could spawn grandchildren that escape it. */
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
    if (new_process_group) flags |= CREATE_NEW_PROCESS_GROUP;

    /* lpApplicationName=NULL — same reason as win_launch (issue #706):
     * let CreateProcessW PATH-resolve cmdline[0] rather than treating
     * the bare prog name as a literal CWD filename. */
    BOOL ok = CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, flags, wenv, NULL, &si, &pi);
    free(cmdline); free(wenv);
    if (!ok) { CloseHandle(job); out._1 = "spawn failed"; return out; }

    AssignProcessToJobObject(job, pi.hProcess);

    HANDLE old_job = g_supervised_job;
    int installed = 0;
    if (forward_signals) {
        g_supervised_job = job;
        SetConsoleCtrlHandler(aether_os_ctrl_handler, TRUE);
        installed = 1;
    }

    ResumeThread(pi.hThread);

    const char* outcome = "exited";
    int exit_code = -1;
    DWORD ms = (timeout_secs > 0) ? (DWORD)timeout_secs * 1000 : INFINITE;
    DWORD w = WaitForSingleObject(pi.hProcess, ms);
    if (w == WAIT_TIMEOUT) {
        TerminateJobObject(job, 124);             /* GNU timeout convention */
        WaitForSingleObject(pi.hProcess, 5000);
        outcome = "timeout";
        exit_code = 124;
    } else if (w == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        exit_code = (int)code;
        outcome = "exited";
    } else {
        outcome = "error";
        exit_code = -1;
    }

    if (installed) {
        SetConsoleCtrlHandler(aether_os_ctrl_handler, FALSE);
        g_supervised_job = old_job;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(job); /* fires KILL_ON_JOB_CLOSE when reap_group was set */

    out._0 = exit_code;
    out._1 = outcome;
    return out;
}

/* os.run_full on Windows. The POSIX side uses a poll(2) loop over the
 * three pipes; Win32 anonymous pipes aren't pollable, so we drain stdout
 * and stderr on dedicated reader threads while the main thread writes
 * stdin — same deadlock-free guarantee, different mechanism. */
typedef struct { const char* _0; const char* _1; int _2; const char* _3; } _tuple_string_string_int_string;

typedef struct {
    HANDLE pipe;
    char*  buf;
    size_t len;
    size_t cap;
    int    oom;
} aether_win_drain_ctx;

/* Read `pipe` to EOF into a doubling buffer. EOF on a Win32 pipe surfaces
 * as ReadFile returning FALSE with ERROR_BROKEN_PIPE once the child's
 * write end is closed — treated as a clean end-of-stream. */
static unsigned __stdcall aether_win_drain_thread(void* arg) {
    aether_win_drain_ctx* c = (aether_win_drain_ctx*)arg;
    char tmp[4096];
    DWORD got = 0;
    for (;;) {
        if (!ReadFile(c->pipe, tmp, sizeof tmp, &got, NULL)) break;
        if (got == 0) break;
        if (c->len + got + 1 > c->cap) {
            size_t nc = c->cap ? c->cap : 4096;
            while (c->len + got + 1 > nc) nc *= 2;
            char* b = (char*)realloc(c->buf, nc);
            if (!b) { c->oom = 1; break; }
            c->buf = b; c->cap = nc;
        }
        memcpy(c->buf + c->len, tmp, got);
        c->len += got;
    }
    return 0;
}

_tuple_string_string_int_string os_run_full_raw(const char* prog, void* argv_list, void* env_list,
                                                const char* stdin_data, int stdin_len) {
    _tuple_string_string_int_string out = {
        aether_os_empty_heap(), aether_os_empty_heap(), -1, "" };
    if (!prog) { out._3 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._3 = "denied by sandbox"; return out; }

    SECURITY_ATTRIBUTES sa; memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa; sa.bInheritHandle = TRUE; sa.lpSecurityDescriptor = NULL;

    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL;
    if (!CreatePipe(&in_r, &in_w, &sa, 0)) { out._3 = "pipe failed"; return out; }
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        CloseHandle(in_r); CloseHandle(in_w);
        out._3 = "pipe failed"; return out;
    }
    if (!CreatePipe(&err_r, &err_w, &sa, 0)) {
        CloseHandle(in_r); CloseHandle(in_w); CloseHandle(out_r); CloseHandle(out_w);
        out._3 = "pipe failed"; return out;
    }
    /* Parent-side ends must not be inherited by the child. */
    SetHandleInformation(in_w,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    wchar_t* cmdline = build_command_line(prog, argv_list);
    wchar_t* wenv = build_environ_block(env_list); /* NULL = inherit */
    if (!cmdline) {
        free(cmdline); free(wenv);
        CloseHandle(in_r); CloseHandle(in_w); CloseHandle(out_r);
        CloseHandle(out_w); CloseHandle(err_r); CloseHandle(err_w);
        out._3 = "argv build failed"; return out;
    }

    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = in_r;
    si.hStdOutput = out_w;
    si.hStdError  = err_w;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);

    /* lpApplicationName=NULL — same reason as win_launch (issue #706):
     * let CreateProcessW PATH-resolve cmdline[0] rather than treating
     * the bare prog name as a literal CWD filename. */
    BOOL ok = CreateProcessW(NULL, cmdline, NULL, NULL, TRUE,
                             CREATE_UNICODE_ENVIRONMENT, wenv, NULL, &si, &pi);
    free(cmdline); free(wenv);
    /* Close the child-side ends in the parent so EOF propagates correctly. */
    CloseHandle(in_r); CloseHandle(out_w); CloseHandle(err_w);
    if (!ok) {
        CloseHandle(in_w); CloseHandle(out_r); CloseHandle(err_r);
        out._3 = "spawn failed"; return out;
    }

    aether_win_drain_ctx octx; memset(&octx, 0, sizeof octx); octx.pipe = out_r;
    aether_win_drain_ctx ectx; memset(&ectx, 0, sizeof ectx); ectx.pipe = err_r;
    HANDLE ot = (HANDLE)_beginthreadex(NULL, 0, aether_win_drain_thread, &octx, 0, NULL);
    HANDLE et = (HANDLE)_beginthreadex(NULL, 0, aether_win_drain_thread, &ectx, 0, NULL);

    size_t total = (stdin_data && stdin_len > 0) ? (size_t)stdin_len : 0;
    size_t off = 0;
    while (off < total) {
        DWORD chunk = (DWORD)((total - off) > 65536 ? 65536 : (total - off));
        DWORD wrote = 0;
        if (!WriteFile(in_w, stdin_data + off, chunk, &wrote, NULL)) break; /* child closed stdin */
        off += wrote;
    }
    CloseHandle(in_w); /* deliver EOF to the child's stdin */

    WaitForSingleObject(pi.hProcess, INFINITE);
    if (ot) { WaitForSingleObject(ot, INFINITE); CloseHandle(ot); }
    if (et) { WaitForSingleObject(et, INFINITE); CloseHandle(et); }
    CloseHandle(out_r); CloseHandle(err_r);

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);

    if (octx.oom || ectx.oom) {
        free(octx.buf); free(ectx.buf);
        out._3 = "alloc failed"; return out;
    }
    if (octx.buf) { octx.buf[octx.len] = '\0'; free((void*)out._0); out._0 = octx.buf; }
    if (ectx.buf) { ectx.buf[ectx.len] = '\0'; free((void*)out._1); out._1 = ectx.buf; }
    out._2 = (int)code;
    return out;
}

/* ipc_parent_channel_raw on Windows — implemented in
 * std/ipc/aether_ipc.c (returns -1 unconditionally; Windows is
 * stub-first per the v1 design). */

#endif // !_WIN32


#endif // AETHER_HAS_FILESYSTEM
