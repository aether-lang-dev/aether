#include "aether_panic.h"
#include "../utils/aether_fatal.h"   /* aether_fatal_flush (#3435) */
/* Header-only use: is_aether_string and the AetherString layout. Nothing
 * from std/string is linked, so a runtime-only link (the WASI panic lib)
 * still resolves. */
#include "../../std/string/aether_string.h"
#include "../aether_exec.h"
#include "aether_frames.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdatomic.h>

// Stack-trace capture (issue #347). Three platform paths:
//
//   - glibc / macOS:  backtrace() + backtrace_symbols() from
//                     <execinfo.h>. Already in libc, no extra link.
//   - Windows / MinGW: CaptureStackBackTrace from kernel32 plus
//                     SymInitialize + SymFromAddr from dbghelp.dll,
//                     loaded when a trace is printed (no link flag).
//   - FreeBSD:        the same two calls, from libexecinfo, which is in
//                     the base system but not linked by default (GhostBSD
//                     ships only its libexecinfo.so.1, with no link-time
//                     symlink). Loaded with dlopen when a trace is first
//                     needed, so no consumer's link line changes.
//   - Everything else (musl, wasm, freestanding): no-op stubs.
//
// Each path captures into the same TLS slot at the call site
// (aether_panic_capture_stack) so the user's caller frames survive
// `-O2` tail-call elimination of the noreturn aether_panic call.
#if (defined(__GLIBC__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)
  #define AETHER_STACK_TRACE_BACKTRACE 1
  #define AETHER_STACK_TRACE_WIN32     0
  #include <execinfo.h>
#elif defined(__FreeBSD__)
  #define AETHER_STACK_TRACE_BACKTRACE 1
  #define AETHER_STACK_TRACE_WIN32     0
  #define AETHER_STACK_TRACE_DLOPEN    1
  #include <dlfcn.h>
#elif defined(_WIN32)
  #define AETHER_STACK_TRACE_BACKTRACE 0
  #define AETHER_STACK_TRACE_WIN32     1
  // <windows.h> defines a `min` / `max` macro that collides with
  // Aether's `string_min` / `string_max` symbols later in the link.
  // NOMINMAX disables those macros without affecting the underlying
  // Win32 API surface.
  #define NOMINMAX
  #include <windows.h>
  #include <dbghelp.h>
#else
  #define AETHER_STACK_TRACE_BACKTRACE 0
  #define AETHER_STACK_TRACE_WIN32     0
#endif

// ---------------------------------------------------------------------------
// Per-thread jmp frame stack
// ---------------------------------------------------------------------------


/* The frames live in the per-thread block aether_frames.h describes,
 * with the executions and the allocation journal. */

/* To the frame a panic was raised for: an execution frame by the
 * __builtin_setjmp buffer in its export stub, any other by its own. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
static void aether_jump_to(AetherJmpFrame* f) {
#if AETHER_EXEC_BUILTIN_SJLJ
    if (f->exec_buf) __builtin_longjmp(f->exec_buf, 1);
#endif
    AETHER_SIGLONGJMP(f->buf, 1);
#if !(defined(__GNUC__) || defined(__clang__))
    abort();
#endif
}

AETHER_TLS_SHARED int g_aether_in_actor_step = 0;
AETHER_TLS_SHARED int g_aether_current_actor_id = -1;
/* #2586: the string a fn-pointer callee handed over; see aether_fnptr_mark
 * in aether_panic.h. Static: the generated code reaches it only through the
 * two functions below (#2687). */
static AETHER_TLS_SHARED const void* g_aether_fnptr_owned = NULL;

void aether_fnptr_mark(const void* s) {
    g_aether_fnptr_owned = s;
}

int aether_fnptr_claim(const void* r) {
    int owned = r && r == g_aether_fnptr_owned;
    g_aether_fnptr_owned = NULL;
    return owned;
}

static AetherDeathHook death_hook = NULL;

/* The copies of the sandbox the panic and the step barrier serve
 * (aether_panic.h, #3438): each registers its unwind of the levels pushed
 * inside a frame a panic leaves (#2955) and its part of an actor step's
 * context (#3393) when it first looks for the runtime copies, before any
 * thread can push one of its levels. Reached through here so that a program
 * that panics but never enforces links no sandbox (aether_sandbox.o and the
 * audit log it writes to), and the scheduler, which every actor program
 * links, none either. With them, the threads with a sandbox level in force
 * (the pointer aether_sandbox.h's check reads, set once the copies are
 * found). These bind as the code that reads them does: under ELF
 * interposition a library that runs the program's panic and step barrier
 * registers its sandbox with the program's. */
atomic_int* _Atomic aether_sandbox_in_force = NULL;
const AetherSandboxHooks* _Atomic aether_sandbox_hooks[AETHER_SANDBOX_HOOKS_MAX];

int aether_sandbox_hooks_add(const AetherSandboxHooks* h) {
    if (!h) return -1;
    for (int i = 0; i < AETHER_SANDBOX_HOOKS_MAX; i++)
        if (atomic_load_explicit(&aether_sandbox_hooks[i], memory_order_acquire) == h) return 0;
    for (int i = 0; i < AETHER_SANDBOX_HOOKS_MAX; i++) {
        const AetherSandboxHooks* none = NULL;
        if (atomic_compare_exchange_strong_explicit(&aether_sandbox_hooks[i], &none, h, memory_order_acq_rel,
                                                    memory_order_acquire) ||
            none == h)
            return 0;
    }
    return -1;
}

void aether_sandbox_hooks_remove(const AetherSandboxHooks* h) {
    for (int i = 0; i < AETHER_SANDBOX_HOOKS_MAX; i++) {
        const AetherSandboxHooks* mine = h;
        atomic_compare_exchange_strong_explicit(&aether_sandbox_hooks[i], &mine, NULL, memory_order_acq_rel,
                                                memory_order_relaxed);
    }
}
/* How this copy joins another copy's sandbox count before its first step
 * (#3438): aether_sandbox.c's, set when that file is loaded, and called by
 * scheduler_init. NULL in a copy without the sandbox, which has no level to
 * see. This copy's own: a constructor sets it, which runs in every image
 * whatever the interposition (aether_sandbox.c). */
AETHER_COPY_LOCAL void (*_Atomic aether_sandbox_join)(void) = NULL;

/* Every copy's levels pushed inside the frame the panic leaves: each copy
 * keeps its own, and the try depth is the one every copy reads. */
static void unwind_sandbox_levels(void) {
    int frames = aether_try_depth();
    for (int i = 0; i < AETHER_SANDBOX_HOOKS_MAX; i++) {
        const AetherSandboxHooks* h = atomic_load_explicit(&aether_sandbox_hooks[i], memory_order_acquire);
        if (h && h->unwind) h->unwind(frames);
    }
}

// ---------------------------------------------------------------------------
// Stack-trace capture (issue #347)
//
// Captures the current call stack via backtrace() and prints a
// filtered, pretty-printed version to stderr before the panic
// fallback aborts. Pretty-printing is a small parser over the
// platform-specific backtrace_symbols() format:
//   - glibc: "binary(symbol+offset) [address]"
//   - macOS: "<frame#>  binary  address  symbol + offset"
// We isolate the symbol token, optionally strip a leading underscore
// (Mach-O convention), and rewrite `aether_<a>_<b>_…` to
// `<a>.<b>.…` so the dotted Aether name reads back at the user
// (`aether_std_string_concat` → `std.string.concat`).
//
// On platforms without backtrace() (musl, Windows, freestanding,
// wasm), the helper is a no-op stub — callers still get the panic
// reason; they just don't get the trace. CaptureStackBackTrace +
// DbgHelp on Windows is a separate follow-up.
// ---------------------------------------------------------------------------

#if AETHER_STACK_TRACE_BACKTRACE

// Per-thread captured backtrace, populated by aether_panic_capture_stack()
// at the codegen call site. Read by aether_panic's fallback printer.
// Capturing at the call site (rather than from inside aether_panic
// itself) is what gives us the user's caller frames under -O2 —
// tail-call + noreturn collapse the caller frame, and backtrace()
// inside aether_panic alone walks an already-truncated stack.
#define AETHER_PANIC_TRACE_MAX 64
static AETHER_TLS_SHARED void* tls_panic_trace[AETHER_PANIC_TRACE_MAX];
static AETHER_TLS_SHARED int   tls_panic_trace_n = 0;

#if defined(AETHER_STACK_TRACE_DLOPEN)
// libexecinfo's backtrace/backtrace_symbols, resolved on first use. glibc's
// own backtrace() dlopens libgcc_s lazily the same way. A failed load (a
// static binary, a sandbox that refuses the dlopen) leaves both NULL, and
// the panic prints its reason without a trace, as on musl.
typedef size_t (*aether_backtrace_fn)(void**, size_t);
typedef char** (*aether_backtrace_symbols_fn)(void* const*, size_t);
static _Atomic(aether_backtrace_fn)         aether_execinfo_backtrace;
static _Atomic(aether_backtrace_symbols_fn) aether_execinfo_symbols;

static void aether_execinfo_load(void) {
    if (atomic_load(&aether_execinfo_backtrace)) return;
    void* h = dlopen("libexecinfo.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libexecinfo.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) return;
    aether_backtrace_symbols_fn sy =
        (aether_backtrace_symbols_fn)dlsym(h, "backtrace_symbols");
    aether_backtrace_fn bt = (aether_backtrace_fn)dlsym(h, "backtrace");
    if (!bt || !sy) return;
    atomic_store(&aether_execinfo_symbols, sy);
    atomic_store(&aether_execinfo_backtrace, bt);
}

static int aether_backtrace(void** frames, int max) {
    aether_execinfo_load();
    aether_backtrace_fn bt = atomic_load(&aether_execinfo_backtrace);
    return bt ? (int)bt(frames, (size_t)max) : 0;
}

static char** aether_backtrace_symbols(void* const* frames, int n) {
    aether_backtrace_symbols_fn sy = atomic_load(&aether_execinfo_symbols);
    return sy ? sy(frames, (size_t)n) : NULL;
}
#else
#define aether_backtrace(frames, max)       backtrace((frames), (max))
#define aether_backtrace_symbols(frames, n) backtrace_symbols((frames), (n))
#endif

void aether_panic_capture_stack(void) {
    tls_panic_trace_n = aether_backtrace(tls_panic_trace, AETHER_PANIC_TRACE_MAX);
}

// Locate the symbol name inside a backtrace_symbols() line. Returns
// a pointer to the start of the symbol within `line`, plus its
// length via `*out_len`. Returns NULL if no symbol could be parsed.
//
// Format families:
//   - glibc ELF: "binary(symbol+0xOFFSET) [0xADDR]"
//                 → symbol is between '(' and '+' (or ')' if no offset)
//   - glibc-no-debug: "binary [0xADDR]"
//                 → no symbol; return NULL
//   - macOS Mach-O: "<n>   binary   0xADDR   symbol + offset"
//                 → symbol is the 4th whitespace-separated field
//   - FreeBSD libexecinfo: "0xADDR <symbol+OFFSET> at binary"
//                 → symbol is between '<' and '+' (or '>')
static const char* aether_locate_symbol(const char* line, size_t* out_len) {
    if (!line || !out_len) return NULL;

#if defined(AETHER_STACK_TRACE_DLOPEN)
    // libexecinfo-shape: "0xADDR <symbol+0xOFF> at /path". Checked before
    // the glibc shape, since the path after "at" may hold a '('.
    const char* langle = strchr(line, '<');
    if (langle) {
        const char* start = langle + 1;
        const char* end   = start;
        while (*end && *end != '+' && *end != '>') end++;
        if (end > start && *end) {
            *out_len = (size_t)(end - start);
            return start;
        }
    }
#endif

    // glibc-shape: look for '('
    const char* lparen = strchr(line, '(');
    if (lparen) {
        const char* start = lparen + 1;
        const char* end   = start;
        while (*end && *end != '+' && *end != ')') end++;
        if (end > start) {
            *out_len = (size_t)(end - start);
            return start;
        }
        // Empty parens (no symbol resolved) — fall through to other shapes.
    }

    // macOS-shape: skip 3 whitespace-separated fields (frame#, binary,
    // address), then the next non-whitespace run is the symbol.
    const char* p = line;
    for (int field = 0; field < 3; field++) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) return NULL;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return NULL;
    const char* start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '+') p++;
    if (p > start) {
        *out_len = (size_t)(p - start);
        return start;
    }

    return NULL;
}

// Pretty-print one symbol token. Writes to `out` (capacity `out_cap`,
// always NUL-terminated). The token may include a `+offset` suffix
// from the backtrace_symbols formatter; we drop it.
static void aether_pretty_symbol(const char* line, char* out, size_t out_cap) {
    if (!line || !out || out_cap == 0) return;
    out[0] = '\0';

    size_t slen = 0;
    const char* sym = aether_locate_symbol(line, &slen);
    if (!sym || slen == 0) return;

    // Skip leading underscore (Mach-O convention on macOS).
    if (sym[0] == '_' && slen > 1) {
        sym++;
        slen--;
    }

    // Match the `aether_` prefix exactly (not `aether` substring) so
    // we only rewrite codegen'd stdlib symbols and don't molest user
    // identifiers that happen to contain the substring.
    int is_aether = (slen > 7 && strncmp(sym, "aether_", 7) == 0);
    const char* body = is_aether ? sym + 7 : sym;
    size_t      blen = is_aether ? slen - 7 : slen;

    size_t pos = 0;
    for (size_t i = 0; i < blen && pos + 1 < out_cap; i++) {
        char c = body[i];
        // Inside aether_-prefixed symbols, underscores are namespace
        // separators — render them as dots. User-code symbols pass
        // through verbatim so that local snake_case names stay
        // recognisable in the trace.
        if (is_aether && c == '_') c = '.';
        out[pos++] = c;
    }
    out[pos] = '\0';
}

// Print up to `frames_to_capture` frames to stderr. Filters out the
// top-of-stack panic plumbing (this function + aether_panic itself)
// so the first frame the user sees is the one that called panic().
//
// Prefers the codegen-captured TLS trace when present (the codegen
// runs aether_panic_capture_stack() before the noreturn aether_panic
// call so the caller frames survive -O2 tail-calls). Falls back to
// a fresh backtrace() for callers that didn't capture (contract
// violations, signal-converted panics, runtime self-checks).
static void aether_print_stack_trace_to_stderr(void) {
    enum { MAX_FRAMES = AETHER_PANIC_TRACE_MAX };
    void*  raw_frames[MAX_FRAMES];
    void** frames;
    int    n;

    if (tls_panic_trace_n > 0) {
        frames = tls_panic_trace;
        n      = tls_panic_trace_n;
    } else {
        n = aether_backtrace(raw_frames, MAX_FRAMES);
        if (n <= 0) return;
        frames = raw_frames;
    }

    char** symbols = aether_backtrace_symbols(frames, n);
    if (!symbols) return;

    fprintf(stderr, "\nStack trace (most recent call first):\n");

    // Skip our own frames: aether_print_stack_trace_to_stderr +
    // aether_panic. Search the symbol list rather than hardcoding
    // the count, so inlining or LTO that fuses one of them away
    // doesn't leave a hole.
    int start = 0;
    for (int i = 0; i < n && i < 4; i++) {
        const char* s = symbols[i];
        if (s && (strstr(s, "aether_panic") || strstr(s, "print_stack_trace"))) {
            start = i + 1;
        }
    }

    int printed = 0;
    char pretty[256];
    for (int i = start; i < n; i++) {
        aether_pretty_symbol(symbols[i], pretty, sizeof(pretty));
        // Drop libc / dyld startup frames — they aren't useful and
        // their names vary by platform.
        if (pretty[0] == '\0') continue;
        if (strcmp(pretty, "start")              == 0) break;
        if (strcmp(pretty, "_start")             == 0) break;
        if (strcmp(pretty, "__libc_start_main")  == 0) break;
        if (strcmp(pretty, "__libc_start_call_main") == 0) break;

        fprintf(stderr, "  %d: %s\n", printed, pretty);
        printed++;

        // Stop on main; everything below is libc/runtime startup.
        if (strcmp(pretty, "main") == 0) break;
    }

    free(symbols);
}

#elif AETHER_STACK_TRACE_WIN32

// Windows path: CaptureStackBackTrace lives in kernel32 (always
// linked), SymFromAddr lives in dbghelp.dll (loaded below when a
// trace is printed). The shape mirrors the POSIX path, TLS-buffered
// frames captured at the call site, walked + pretty-printed in the
// fallback printer — so the rest of the panic plumbing is identical.

#define AETHER_PANIC_TRACE_MAX 64
static AETHER_TLS_SHARED PVOID tls_panic_trace[AETHER_PANIC_TRACE_MAX];
static AETHER_TLS_SHARED USHORT tls_panic_trace_n = 0;
static AETHER_TLS_SHARED int tls_panic_sym_initialised = 0;

/* dbghelp is loaded when a trace is printed, not linked: every export runs
 * in a panic frame (#2956), so this object is in every library, and a C
 * host linking one with libaether.a would otherwise need -ldbghelp. */
typedef DWORD (WINAPI *aether_sym_get_options_fn)(void);
typedef DWORD (WINAPI *aether_sym_set_options_fn)(DWORD);
typedef BOOL (WINAPI *aether_sym_initialize_fn)(HANDLE, PCSTR, BOOL);
typedef BOOL (WINAPI *aether_sym_from_addr_fn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
static aether_sym_from_addr_fn aether_sym_from_addr = NULL;

static void aether_load_dbghelp(HANDLE proc) {
    HMODULE h = LoadLibraryA("dbghelp.dll");
    if (!h) return;
    FARPROC get = GetProcAddress(h, "SymGetOptions");
    FARPROC set = GetProcAddress(h, "SymSetOptions");
    FARPROC init = GetProcAddress(h, "SymInitialize");
    FARPROC from = GetProcAddress(h, "SymFromAddr");
    if (!get || !set || !init || !from) return;
    aether_sym_get_options_fn get_options;
    aether_sym_set_options_fn set_options;
    aether_sym_initialize_fn initialize;
    memcpy(&get_options, &get, sizeof(get_options));
    memcpy(&set_options, &set, sizeof(set_options));
    memcpy(&initialize, &init, sizeof(initialize));
    set_options(get_options() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    initialize(proc, NULL, TRUE);
    memcpy(&aether_sym_from_addr, &from, sizeof(aether_sym_from_addr));
}

void aether_panic_capture_stack(void) {
    // Skip 1 frame (this function itself); the next frame is the
    // codegen call site, which is the user's panic statement.
    tls_panic_trace_n = CaptureStackBackTrace(
        1, AETHER_PANIC_TRACE_MAX, tls_panic_trace, NULL);
}

// Pretty-print the same way the POSIX path does: strip the
// `aether_` prefix on namespace symbols, dot-separate the rest,
// pass user-code symbols verbatim.
static void aether_pretty_symbol_win(const char* sym, char* out, size_t out_cap) {
    if (!sym || !out || out_cap == 0) return;
    out[0] = '\0';

    size_t slen = strlen(sym);
    int is_aether = (slen > 7 && strncmp(sym, "aether_", 7) == 0);
    const char* body = is_aether ? sym + 7 : sym;
    size_t      blen = is_aether ? slen - 7 : slen;

    size_t pos = 0;
    for (size_t i = 0; i < blen && pos + 1 < out_cap; i++) {
        char c = body[i];
        if (is_aether && c == '_') c = '.';
        out[pos++] = c;
    }
    out[pos] = '\0';
}

static void aether_print_stack_trace_to_stderr(void) {
    if (tls_panic_trace_n == 0) {
        // No call-site capture (contract violation, signal-converted
        // panic, runtime self-check). Capture fresh from here; same
        // -O2 caveats as POSIX apply.
        tls_panic_trace_n = CaptureStackBackTrace(
            1, AETHER_PANIC_TRACE_MAX, tls_panic_trace, NULL);
        if (tls_panic_trace_n == 0) return;
    }

    HANDLE proc = GetCurrentProcess();
    if (!tls_panic_sym_initialised) {
        // Lazy init — SymInitialize is process-global but cheap and
        // safe to call once per thread that actually needs symbols.
        // SYMOPT_LOAD_LINES could be added for line numbers; deferred
        // (PDB availability is implementation-defined under MinGW).
        aether_load_dbghelp(proc);
        tls_panic_sym_initialised = 1;
    }

    fprintf(stderr, "\nStack trace (most recent call first):\n");

    // SYMBOL_INFO has a flexible array tail for the name; reserve
    // 256 bytes for the symbol name plus the struct header.
    enum { NAME_MAX = 255 };
    union {
        SYMBOL_INFO info;
        char        bytes[sizeof(SYMBOL_INFO) + NAME_MAX + 1];
    } sym_buf;
    SYMBOL_INFO* sym = &sym_buf.info;

    int printed = 0;
    char pretty[256];
    for (int i = 0; i < tls_panic_trace_n; i++) {
        DWORD64 addr = (DWORD64)(uintptr_t)tls_panic_trace[i];
        memset(&sym_buf, 0, sizeof(sym_buf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = NAME_MAX;
        DWORD64 disp = 0;
        BOOL ok = aether_sym_from_addr ? aether_sym_from_addr(proc, addr, &disp, sym) : FALSE;

        if (ok && sym->Name[0] != '\0') {
            // Skip our own panic-handler frames so the first thing
            // the user sees is the frame that called panic().
            if (strstr(sym->Name, "aether_panic") ||
                strstr(sym->Name, "print_stack_trace")) {
                continue;
            }

            aether_pretty_symbol_win(sym->Name, pretty, sizeof(pretty));
            if (pretty[0] == '\0') {
                // Symbol was all-prefix and got eaten by the
                // pretty-printer — fall through to the address path.
                snprintf(pretty, sizeof(pretty), "0x%llx",
                         (unsigned long long)addr);
            }

            // Stop on C runtime startup frames — same heuristic as
            // POSIX. MinGW main thread starts in main →
            // __tmainCRTStartup → BaseThreadInitThunk →
            // RtlUserThreadStart.
            if (strcmp(pretty, "BaseThreadInitThunk")  == 0) break;
            if (strcmp(pretty, "RtlUserThreadStart")   == 0) break;
            if (strcmp(pretty, "__tmainCRTStartup")    == 0) break;

            fprintf(stderr, "  %d: %s\n", printed, pretty);
            printed++;

            if (strcmp(pretty, "main") == 0) break;
        } else {
            // No PDB / no symbol info for this address. Show the
            // raw address so the trace is never empty — much better
            // than silently swallowing the frame, especially when
            // every frame in a stripped MinGW build hits this path.
            fprintf(stderr, "  %d: 0x%llx\n", printed,
                    (unsigned long long)addr);
            printed++;
        }
    }
}

#else  // !AETHER_STACK_TRACE_BACKTRACE && !AETHER_STACK_TRACE_WIN32

void aether_panic_capture_stack(void) {
    // No-op on platforms without backtrace() or CaptureStackBackTrace
    // (musl, Emscripten wasm, freestanding bare-metal). Tracing is
    // best-effort diagnostic info — the panic path itself still works.
}

static void aether_print_stack_trace_to_stderr(void) {
    // Nothing captured, nothing to print.
}

#endif

// ---------------------------------------------------------------------------
// Panic entry point
// ---------------------------------------------------------------------------

/* The text of a panic reason (#2340). A message built at run time reaches
 * the runtime as the AetherString codegen made it -- the catch lowering
 * adopts that object as its binding -- so printing it as a C string would
 * print the header. A literal is a plain char* and is its own text. */
const char* aether_panic_reason_text(const char* reason) {
    if (!reason) return "unknown";
    if (is_aether_string(reason)) return ((const AetherString*)reason)->data;
    return reason;
}

void aether_panic(const char* reason) {
    aether_panic_owned(reason, NULL);
}

/* The reason an interruption of a library call carries (#2956,
 * aether_exec.c), by its outcome. */
const char* aether_exec_reason(int outcome) {
    switch (outcome) {
    case AETHER_DEADLINE:      return "the call's deadline passed";
    case AETHER_CANCELLED:     return "the call was cancelled";
    case AETHER_MEMORY_CAP:    return "the call went past the memory cap";
    case AETHER_OUT_OF_MEMORY: return "the call ran out of memory";
    case AETHER_INCOMPATIBLE:  return "the library was made for another runtime";
    default:                   return "panic";
    }
}

/* An allocation the code cannot go on without failed (#2956). Inside an
 * execution the call ends with AETHER_OUT_OF_MEMORY, an interruption a
 * `try` does not catch, its reason a static string: nothing on the way
 * allocates. Outside one it is the panic `what` it always was. */
void aether_out_of_memory(const char* what) {
    AetherFrames* F = aether_frames_here();
    if (F->exec_depth > F->exec_floor) {   /* an execution of this code's (#3392) */
        F->interrupting = AETHER_OUT_OF_MEMORY;
        aether_panic(aether_exec_reason(AETHER_OUT_OF_MEMORY));
    }
    aether_panic(what);
}

/* A `try` caught an interruption (aether_exec_interrupting()): it has done
 * its cleanup, and the interruption goes on to the next frame out, which
 * ends with the execution's. Here, with the panic it raises again, since
 * every program with a `try` calls it. */
void aether_exec_reraise(void) {
    aether_try_pop();
    aether_panic(aether_exec_reason(aether_frames_here()->interrupting));
}

void aether_panic_owned(const char* reason, void (*release)(const void*)) {
    if (!reason) { reason = "panic: (null)"; release = NULL; }

    AetherJmpFrame* f = aether_current_frame();
    if (f) {
        f->reason = reason;
        f->reason_release = release;
        // The sandbox levels pushed inside the frame are left with it, in
        // the runtime, whichever code catches (#2955): a catch in code that
        // never enforces, or an actor's barrier, restored nothing, and the
        // thread stayed held to grants freed with the block. Before the
        // drain, which may free them.
        unwind_sandbox_levels();
        // Free the innermost frame's still-live journaled allocations
        // before the jump skips their deferred frees (aether_unwind.c).
        aether_unwind_drain_current();
        aether_jump_to(f);
        // unreachable
    }

    // No user-level try/catch. Print the reason + a filtered stack
    // trace to stderr so the caller sees the call path that led here,
    // then abort. In an actor context the scheduler's own frame will
    // catch this before we get here — only non-actor threads with no
    // frame reach the fallback. Suppress the trace via
    // AETHER_STACK_TRACE=0 if the noise gets in the way (e.g. tests
    // that diff stderr line-for-line).
    //
    // The program's own lines first (#3435): stdout is fully buffered when it
    // is a file or a pipe, and abort() flushes no stdio stream on glibc, so
    // what the program printed last was lost behind this message.
    aether_fatal_flush();
    fprintf(stderr, "aether: panic outside any try/catch or actor: %s\n",
            aether_panic_reason_text(reason));
    const char* trace_env = getenv("AETHER_STACK_TRACE");
    if (!trace_env || strcmp(trace_env, "0") != 0) {
        aether_print_stack_trace_to_stderr();
    }
    abort();
}

// ---------------------------------------------------------------------------
// Signal handlers (opt-in via AETHER_CATCH_SIGNALS=1)
// ---------------------------------------------------------------------------
//
// Caveats documented in docs/actor-concurrency.md ("Panic and Stack
// Traces"): converting a native fault into a
// panic is best-effort. A SIGSEGV mid-enqueue may leave queue state
// inconsistent. This path exists so the process survives *some* native
// faults during development and testing, not as a production replacement
// for memory safety.
//
// Windows has no sigaction / SA_SIGINFO / SIGBUS; Win32 uses SEH for native
// faults, a different recovery model entirely. Emscripten and WASI wasm
// targets don't expose POSIX signal delivery at all. Freestanding / bare-metal
// targets (arm-none-eabi newlib under -ffreestanding) have no POSIX signal
// surface either. On all four, the installer is a no-op stub so the rest
// of the panic path (panic()/try/catch via setjmp) still works; only the
// "convert SIGSEGV into a panic" feature is POSIX-only.
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !defined(__wasi__) && !(defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 0)

static void aether_sig_handler(int sig, siginfo_t* info, void* ucontext) {
    (void)info;
    (void)ucontext;

    // Only attempt recovery if we're in an actor step. Otherwise restore
    // default handler and let the OS take us down — this avoids re-entering
    // the signal handler if the scheduler itself faulted.
    if (!g_aether_in_actor_step) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }

    AetherJmpFrame* f = aether_current_frame();
    if (!f) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }

    const char* reason;
    switch (sig) {
        case SIGSEGV: reason = "signal: SIGSEGV (invalid memory access)"; break;
        case SIGFPE:  reason = "signal: SIGFPE (arithmetic fault)";       break;
        case SIGBUS:  reason = "signal: SIGBUS (bus error)";              break;
        default:      reason = "signal: unknown";                         break;
    }
    f->reason = reason;
    f->reason_release = NULL;
    unwind_sandbox_levels();
    aether_jump_to(f);
}

void aether_panic_install_signal_handlers(void) {
    const char* env = getenv("AETHER_CATCH_SIGNALS");
    if (!env || strcmp(env, "1") != 0) return;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = aether_sig_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);

    // SA_NODEFER is needed because we longjmp out of the handler. The
    // kernel's automatic "add this signal to the mask on handler entry"
    // behaviour is disabled, so the mask never changes at signal entry
    // and there is nothing for the scheduler-side jmp to save/restore.
    // That is why AETHER_SIGSETJMP expands to the fast _setjmp on POSIX
    // (no signal-mask syscall) with no loss of correctness here.

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
}

#else  // Windows / Emscripten / WASI / freestanding: no POSIX signal installer.

void aether_panic_install_signal_handlers(void) {
    // Intentional no-op. On Windows the SIGSEGV-to-panic conversion path
    // would require SEH/__try, which is a separate design. Emscripten/WASI
    // wasm and freestanding bare-metal targets have no POSIX signal
    // delivery at all. Callers that use plain panic() / try / catch
    // still work unchanged on all four.
}

#endif  // !_WIN32 && !__EMSCRIPTEN__ && !__wasi__ && hosted

// ---------------------------------------------------------------------------
// Death hook
// ---------------------------------------------------------------------------

void aether_set_on_actor_death(AetherDeathHook fn) {
    death_hook = fn;
}

void aether_fire_death_hook(int actor_id, const char* reason) {
    AetherDeathHook h = death_hook;
    if (h) h(actor_id, reason ? aether_panic_reason_text(reason) : "unknown");
}

/* aether_panic.h (#3403, #3404, #3415). What the frame caught is told by
 * what its code raised, not by the state the thread is in after it: an
 * interruption already on its way when the code began (its unwind runs
 * defers and journal drains, and one that ends an actor runs the actor's
 * drops here) is not the code's. It is put aside while the code runs and
 * given back after it, so an ordinary panic in the code is reported as one
 * and the interruption goes on as it was. Read after the catch, it made the
 * code's ordinary panic the interruption: taken off its way, the unwind went
 * on as an ordinary panic, which a `try` caught and the call's frame
 * recorded as PANICKED. */
int aether_contain(void (*fn)(void*), void* arg, const char** reason,
                   void (**reason_release)(const void*)) {
    AetherFrames* F = aether_frames_here();
    int on_its_way = F->interrupting;
    F->interrupting = 0;
    AetherJmpFrame* f = aether_try_push();
    if (AETHER_SIGSETJMP(f->buf, 1) == 0) {
        fn(arg);
        aether_try_pop();
        F->interrupting = on_its_way;
        return 0;
    }
    const char* r = f->reason;
    void (*rel)(const void*) = f->reason_release;
    aether_try_pop();
    if (F->interrupting) {
        /* An interruption of the call this code runs inside, raised in it (a
         * drop function's loop past the call's deadline, in a finalize the
         * call runs): not this frame's to end, nor to report as a panic.
         * The call ends at its own next safe point, or at its return. Left
         * on its way, it turned the call's next ordinary panic into it. */
        if (rel) rel(r);
        aether_exec_defer_interruption();
        F->interrupting = on_its_way;
        return 2;
    }
    F->interrupting = on_its_way;
    *reason = r;
    *reason_release = rel;
    return 1;
}

typedef struct { int id; const char* reason; } DeathHookCall;

static void death_hook_body(void* p) {
    DeathHookCall* c = (DeathHookCall*)p;
    aether_fire_death_hook(c->id, c->reason);
}

void aether_fire_death_hook_contained(int actor_id, const char* reason) {
    if (!death_hook) return;
    DeathHookCall c = { actor_id, reason };
    const char* r = NULL;
    void (*rel)(const void*) = NULL;
    if (aether_contain(death_hook_body, &c, &r, &rel) == 1) {
        fprintf(stderr, "aether: the death hook of actor %d panicked: %s\n", actor_id,
                aether_panic_reason_text(r));
        if (rel) rel(r);
    }
}
