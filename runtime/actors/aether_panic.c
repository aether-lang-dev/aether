#include "aether_panic.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

// Stack-trace capture (issue #347). Three platform paths:
//
//   - glibc / macOS:  backtrace() + backtrace_symbols() from
//                     <execinfo.h>. Already in libc, no extra link.
//   - Windows / MinGW: CaptureStackBackTrace from kernel32 plus
//                     SymInitialize + SymFromAddr from dbghelp.dll.
//                     The Makefile + ae.c link `-ldbghelp`.
//   - Everything else (musl, wasm, freestanding): no-op stubs.
//
// Each path captures into the same TLS slot at the call site
// (aether_panic_capture_stack) so the user's caller frames survive
// `-O2` tail-call elimination of the noreturn aether_panic call.
#if (defined(__GLIBC__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)
  #define AETHER_STACK_TRACE_BACKTRACE 1
  #define AETHER_STACK_TRACE_WIN32     0
  #include <execinfo.h>
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

typedef struct {
    AetherJmpFrame frames[AETHER_PANIC_MAX_DEPTH];
    int depth;  // number of frames currently live; innermost is frames[depth-1]
} AetherJmpStack;

/* AETHER_TLS_SHARED rather than AETHER_TLS: aether_panic.o is
 * pulled into both the main `ae` executable AND `--emit=lib` .so
 * outputs (codegen emits aether_panic_capture_stack + aether_panic
 * at every user `panic("...")` call site). Initial-Exec TLS
 * relocations don't work inside shared objects; Global-Dynamic
 * works in both contexts with a tiny GOT-indirection overhead.
 * See AETHER_TLS_SHARED in runtime/utils/aether_compiler.h. */
static AETHER_TLS_SHARED AetherJmpStack tls_stack = { .depth = 0 };

AETHER_TLS_SHARED int g_aether_in_actor_step = 0;
AETHER_TLS_SHARED int g_aether_current_actor_id = -1;

static AetherDeathHook death_hook = NULL;

AetherJmpFrame* aether_try_push(void) {
    if (tls_stack.depth >= AETHER_PANIC_MAX_DEPTH) {
        fprintf(stderr, "aether: try/catch nesting exceeded %d, aborting\n",
                AETHER_PANIC_MAX_DEPTH);
        abort();
    }
    AetherJmpFrame* f = &tls_stack.frames[tls_stack.depth++];
    f->reason = NULL;
    aether_unwind_enter_frame();
    return f;
}

void aether_try_pop(void) {
    if (tls_stack.depth <= 0) {
        // Defensive: popping an empty stack means codegen/runtime mismatch.
        fprintf(stderr, "aether: aether_try_pop on empty stack\n");
        abort();
    }
    aether_unwind_exit_frame();
    tls_stack.depth--;
}

AetherJmpFrame* aether_current_frame(void) {
    if (tls_stack.depth == 0) return NULL;
    return &tls_stack.frames[tls_stack.depth - 1];
}

int aether_try_depth(void) {
    return tls_stack.depth;
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

void aether_panic_capture_stack(void) {
    tls_panic_trace_n = backtrace(tls_panic_trace, AETHER_PANIC_TRACE_MAX);
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
static const char* aether_locate_symbol(const char* line, size_t* out_len) {
    if (!line || !out_len) return NULL;

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
        n = backtrace(raw_frames, MAX_FRAMES);
        if (n <= 0) return;
        frames = raw_frames;
    }

    char** symbols = backtrace_symbols(frames, n);
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
// linked), SymFromAddr lives in dbghelp.dll (Makefile + ae.c add
// -ldbghelp). The shape mirrors the POSIX path — TLS-buffered
// frames captured at the call site, walked + pretty-printed in the
// fallback printer — so the rest of the panic plumbing is identical.

#define AETHER_PANIC_TRACE_MAX 64
static AETHER_TLS_SHARED PVOID tls_panic_trace[AETHER_PANIC_TRACE_MAX];
static AETHER_TLS_SHARED USHORT tls_panic_trace_n = 0;
static AETHER_TLS_SHARED int tls_panic_sym_initialised = 0;

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
        SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(proc, NULL, TRUE);
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
        BOOL ok = SymFromAddr(proc, addr, &disp, sym);

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

void aether_panic(const char* reason) {
    if (!reason) reason = "panic: (null)";

    AetherJmpFrame* f = aether_current_frame();
    if (f) {
        f->reason = reason;
        // Free the innermost frame's still-live journaled allocations
        // before the jump skips their deferred frees (aether_unwind.c).
        aether_unwind_drain_current();
        AETHER_SIGLONGJMP(f->buf, 1);
        // unreachable
    }

    // No user-level try/catch. Print the reason + a filtered stack
    // trace to stderr so the caller sees the call path that led here,
    // then abort. In an actor context the scheduler's own frame will
    // catch this before we get here — only non-actor threads with no
    // frame reach the fallback. Suppress the trace via
    // AETHER_STACK_TRACE=0 if the noise gets in the way (e.g. tests
    // that diff stderr line-for-line).
    fprintf(stderr, "aether: panic outside any try/catch or actor: %s\n", reason);
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
// faults, a different recovery model entirely. Emscripten's wasm target
// doesn't expose POSIX signal delivery at all. Freestanding / bare-metal
// targets (arm-none-eabi newlib under -ffreestanding) have no POSIX signal
// surface either. On all three, the installer is a no-op stub so the rest
// of the panic path (panic()/try/catch via setjmp) still works; only the
// "convert SIGSEGV into a panic" feature is POSIX-only.
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !(defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 0)

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
    AETHER_SIGLONGJMP(f->buf, 1);
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

#else  // Windows / Emscripten / freestanding: no POSIX signal installer.

void aether_panic_install_signal_handlers(void) {
    // Intentional no-op. On Windows the SIGSEGV-to-panic conversion path
    // would require SEH/__try, which is a separate design. Emscripten
    // wasm and freestanding bare-metal targets have no POSIX signal
    // delivery at all. Callers that use plain panic() / try / catch
    // still work unchanged on all three.
}

#endif  // !_WIN32 && !__EMSCRIPTEN__ && hosted

// ---------------------------------------------------------------------------
// Death hook
// ---------------------------------------------------------------------------

void aether_set_on_actor_death(AetherDeathHook fn) {
    death_hook = fn;
}

void aether_fire_death_hook(int actor_id, const char* reason) {
    AetherDeathHook h = death_hook;
    if (h) h(actor_id, reason ? reason : "unknown");
}
