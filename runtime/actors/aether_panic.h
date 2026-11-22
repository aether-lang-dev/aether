#ifndef AETHER_PANIC_H
#define AETHER_PANIC_H

// Go-style panic / recover for Aether actors.
//
// Surface:
//   - User code:   panic("reason") unwinds via aether_panic().
//                  try { ... } catch e { ... } codegens to a push + sigsetjmp
//                  pair; the catch body runs with `e` bound to the reason.
//   - Scheduler:   wraps each actor step() in a sigsetjmp barrier using the
//                  same push/pop primitives. On longjmp, the actor is marked
//                  dead and the on_actor_death hook fires. Other actors
//                  keep running.
//   - Signals:     AETHER_CATCH_SIGNALS=1 at process start installs
//                  SIGSEGV/SIGFPE/SIGBUS handlers that convert native faults
//                  into aether_panic(). Off by default because a native
//                  fault mid-enqueue can leave runtime state inconsistent.
//
// Nested try is supported via a per-thread stack of frames. Each try / each
// scheduler step() wrapper pushes one frame on entry and pops on normal exit
// or after catching.
//
// The scheduler and codegen both construct the pattern manually at the call
// site — sigsetjmp must lexically enclose the work being guarded, so we
// can't wrap it in a helper function. aether_try_push() returns a pointer
// to the newly-allocated frame; the caller then calls sigsetjmp(frame->buf, 1).

#include <setjmp.h>
#include <stdatomic.h>
#include <stdlib.h>   /* abort(), for the __wasi__ no-unwind arm below */
#include "../utils/aether_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

// The step-level panic barrier runs on every actor step, so it has to
// be cheap. Plain register-save setjmp/longjmp is a handful of
// instructions on modern CPUs. The signal-mask-preserving variants —
// sigsetjmp(buf, 1) and macOS's default setjmp() — call
// sigprocmask under the hood, which is a syscall plus context switch,
// orders of magnitude more expensive per call. On a message-heavy
// benchmark that invokes the barrier on every actor step, paying the
// signal-mask variant per step is a large regression end-to-end. Run
// benchmarks/cross-language/aether/ping_pong after any change in this
// area to verify.
//
// So:
//   - POSIX (Linux, macOS, BSDs): use _setjmp/_longjmp explicitly.
//     Both are in POSIX.1-2001 XSI, and they skip the signal-mask
//     save unconditionally. This is the fast path.
//   - mingw-w64 x86_64: _setjmp(buf, NULL), the non-unwinding form (see
//     its arm below for why the default SEH-unwinding setjmp is wrong here).
//   - Other Windows / Emscripten / newlib and picolibc (ESP-IDF) /
//     freestanding: plain setjmp/longjmp. Win32 has no POSIX signal
//     semantics so setjmp is already fast; wasm and the embedded libcs
//     have no sigjmp_buf at all, and the embedded ones no _longjmp.
//
// What we lose: signal-mask preservation during the opt-in
// AETHER_CATCH_SIGNALS=1 SIGSEGV-recovery path. That path is already
// documented as "best-effort, not production-safe" (a SIGSEGV mid-
// enqueue can leave queue state inconsistent), and user-code
// panic() / try / catch doesn't need signal-mask semantics at all.
//
// The aether_sigjmp_buf typedef stays so call sites read uniformly;
// on every target it's now jmp_buf under the hood.
typedef jmp_buf aether_sigjmp_buf;
#if defined(__wasi__)
  /* wasm32-wasi has NO usable setjmp/longjmp, in either spelling.
   *
   * WASI is hosted (__STDC_HOSTED__ == 1) and does not define
   * __EMSCRIPTEN__, so without this arm it falls into the POSIX branch below
   * and gets _setjmp/_longjmp -- which wasi-libc declares but never
   * implements. That is a LINK error, not a compile error, so it surfaces
   * only at the very end of a cross build:
   *
   *     wasm-ld: error: libaether.a(aether_panic.o): undefined symbol: _longjmp
   *
   * The plain setjmp/longjmp arm is no better: wasi-libc's <setjmp.h> is a
   * hard `#error` telling you to compile with `-mllvm -wasm-enable-sjlj` and
   * use an engine implementing the exception-handling proposal. Measured on
   * zig 0.16.0, passing that flag does not help either -- the #error fires
   * before the pass ever runs. Real support needs the WebAssembly
   * exception-handling proposal in both toolchain and engine.
   *
   * So: do not unwind. SETJMP always takes the first-return arm, and LONGJMP
   * traps.
   *
   * THE CONSEQUENCE, PLAINLY: on wasi, panic() / try / catch are fail-stop
   * rather than recoverable -- a panic traps the instance instead of
   * unwinding to the nearest catch. That is a real semantic reduction, and
   * it is documented in docs/build-system.md alongside the target's other
   * caveats.
   *
   * It is still strictly better than the alternative, which is that wasi
   * cannot link at all. Nothing silently mis-executes: abort() is a trap the
   * host observes, not a fallthrough into a half-unwound stack. A catch block
   * that never runs is visible; one that runs on a corrupt stack is not. */
  #define AETHER_SIGSETJMP(buf, savemask) ((void)(buf), 0)
  #define AETHER_SIGLONGJMP(buf, val)     (((void)(buf)), ((void)(val)), abort())
#elif defined(__MINGW32__) && defined(__x86_64__)
  /* mingw-w64 on x86_64: setjmp is `_setjmp(buf, __builtin_frame_address(0))`,
   * and a non-NULL frame makes longjmp a structured-exception unwind
   * (RtlUnwind walking every frame back to the setjmp). A panic needs no
   * unwind: cleanup is the allocation journal's, as on POSIX. And the unwind
   * itself faulted intermittently inside RtlVirtualUnwind2, crashing a
   * caught panic (std/sort/test_sort.ae, about one run in seven under load).
   * A NULL frame is mingw-w64's documented request for the plain register
   * restore, the same semantics _setjmp/_longjmp give on POSIX. */
  #define AETHER_SIGSETJMP(buf, savemask) _setjmp((buf), NULL)
  #define AETHER_SIGLONGJMP(buf, val)     longjmp((buf), (val))
#elif defined(_WIN32) || defined(__EMSCRIPTEN__) || defined(__NEWLIB__) || defined(__PICOLIBC__) || \
      (defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 0)
  /* newlib and picolibc (ESP-IDF, most embedded toolchains) are hosted but
   * declare no _longjmp, and their setjmp saves no signal mask: there is
   * no mask to save. */
  #define AETHER_SIGSETJMP(buf, savemask) setjmp(buf)
  #define AETHER_SIGLONGJMP(buf, val)     longjmp((buf), (val))
#else
  #define AETHER_SIGSETJMP(buf, savemask) _setjmp(buf)
  #define AETHER_SIGLONGJMP(buf, val)     _longjmp((buf), (val))
#endif

// An export's execution frame (aether_exec.h) saves its context with GCC's
// __builtin_setjmp, which keeps the frame pointer, stack pointer and resume
// address (the compiler saves the rest in the stub's own frame), where the
// C library's setjmp saves every callee-saved register and, on Win64, ten
// XMM registers: 0.57 ns against 2.85 ns on MinGW. A panic reaches it with
// __builtin_longjmp, made by a function the stub's own translation unit
// defines (the frame's exec_jump), never by the runtime: the two builtins
// share a buffer whose layout depends on -fcf-protection (with it, slot 2
// holds the shadow-stack pointer and slot 3 the stack pointer), and the
// library's C and the runtime are often compiled with different settings. x86 and x86-64 only: clang has no such builtins on
// AArch64 (macOS arm64), where the C library's setjmp is cheap anyway, and
// wasm has no unwinding; there the stub uses AETHER_SIGSETJMP on a frame's
// buf instead.
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__)) && \
    !defined(__wasm__) && !defined(__EMSCRIPTEN__)
  #define AETHER_EXEC_BUILTIN_SJLJ 1
#else
  #define AETHER_EXEC_BUILTIN_SJLJ 0
#endif

// Maximum nesting depth of try/catch (and scheduler step barriers) per
// thread. 32 covers any realistic handler; the limit lets the stack live
// in TLS without dynamic allocation.
#define AETHER_PANIC_MAX_DEPTH 32

typedef struct AetherJmpFrame {
    aether_sigjmp_buf buf;
    const char* reason;   // written by aether_panic() just before siglongjmp
    // Non-NULL when the catcher owns `reason` (a message built at run
    // time, e.g. an interpolated panic): the function that releases it.
    // NULL for a borrowed reason (a literal, a static buffer). Set by
    // aether_panic_owned(); the catch lowering adopts it as the binding's
    // heap flag, and the actor barrier releases it after the death hook.
    void (*reason_release)(const void*);
    // An execution frame entered by aether_exec_enter_at (aether_exec.h):
    // the __builtin_setjmp buffer in the export stub's own frame, which a
    // panic reaches with __builtin_longjmp. NULL for every other frame,
    // which a panic reaches through `buf`. Appended: the fields above keep
    // their offsets.
    void** exec_buf;
    // The stub's own __builtin_longjmp to exec_buf (runtime ABI revision 5):
    // compiled with the stub, so with the flags that laid out the buffer.
    void (*exec_jump)(void** buf);
} AetherJmpFrame;

// Push a new frame onto the current thread's stack and return it. The
// caller is expected to immediately call sigsetjmp(frame->buf, 1). If
// sigsetjmp returns non-zero, a panic has unwound to this frame; the
// reason is in frame->reason.
AetherJmpFrame* aether_try_push(void);

// Pop the innermost frame. Call this on both normal exit (no panic) and
// after handling a caught panic — the frame is consumed either way.
void aether_try_pop(void);

// Innermost live frame on this thread, or NULL. Used by aether_panic() to
// find its target, and by signal handlers.
AetherJmpFrame* aether_current_frame(void);

// Current nesting depth. Exposed for tests.
int aether_try_depth(void);

// User-visible panic entry point. Siglongjmps to the innermost frame with
// reason attached. If no frame exists, prints to stderr and aborts
// (protects non-actor threads that haven't set up a barrier).
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void aether_panic(const char* reason);

// aether_panic() for a reason the catcher takes ownership of: `release`
// frees it (codegen passes the heap-string free for an interpolated or
// otherwise heap-built message). With no frame to catch it the process
// aborts, so the reason is never released there.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void aether_panic_owned(const char* reason, void (*release)(const void*));

// An allocation the code cannot go on without failed (#2956): inside a
// library call's execution the call ends with AETHER_OUT_OF_MEMORY (a
// `try` does not catch it, and nothing on the way allocates); elsewhere it
// is aether_panic(what). Does not return.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void aether_out_of_memory(const char* what);

// Install SIGSEGV/SIGFPE/SIGBUS handlers that convert native faults into
// panics. No-op unless AETHER_CATCH_SIGNALS=1 is set in the environment.
// Call once at process init.
void aether_panic_install_signal_handlers(void);

// Stack-trace capture for the codegen panic path (issue #347). Codegen
// emits this immediately before `aether_panic(...)` so the trace is
// taken with the caller's frames still on the stack — calling
// backtrace() from inside aether_panic itself loses those frames
// under -O2 because tail-call elimination collapses the caller. The
// captured frames live in TLS until the panic fallback prints them.
//
// On platforms without execinfo.h (musl, Windows, wasm,
// freestanding) this is a no-op stub. Tracing is best-effort
// diagnostic info — never required for correct unwinding.
void aether_panic_capture_stack(void);

// Death notification. Fn is invoked with (actor_id, reason) after an actor
// step() unwinds; `reason` is valid only for the call (copy it to keep
// it). NULL clears. Single global slot — if you need fan-out,
// dispatch yourself.
typedef void (*AetherDeathHook)(int actor_id, const char* reason);
void aether_set_on_actor_death(AetherDeathHook fn);

// The text of a caught panic's reason: a reason built at run time is an
// AetherString, a literal its own text (#2340). "unknown" for NULL.
const char* aether_panic_reason_text(const char* reason);

// Called by the scheduler after catching a panic. Fires the on_actor_death
// hook if one is set. Separated from the push/pop primitives so scheduler
// code can decide its own ordering (e.g. mark actor dead first, then fire
// the hook).
void aether_fire_death_hook(int actor_id, const char* reason);

// A containment frame (#3403, #3404): fn(arg) runs in a frame of its own,
// so a panic in it ends there and never jumps past the caller's own cleanup.
// 0: fn returned. 1: it panicked; *reason and *reason_release are the
// panic's, the caller's to report and release. 2: an interruption of the
// library call this code runs inside was raised in it (#3404): it is not the
// frame's to end, and ends the call at the call's next safe point or its
// return, as a stop noted for the call does; nothing to report. An
// interruption already on its way when fn begins (an unwind's defer that
// ends an actor runs its drops here) is not fn's: it is put aside while fn
// runs and given back after, so an ordinary panic in fn is 1 and the
// interruption goes on as it was (#3415). The step barrier runs what follows
// a panicking handler through it, and an ending actor's drops
// (aether_message_drop_at_end).
int aether_contain(void (*fn)(void*), void* arg, const char** reason,
                   void (**reason_release)(const void*));
// The death hook, inside a containment frame: a hook that panics is
// reported on stderr and the caller goes on.
void aether_fire_death_hook_contained(int actor_id, const char* reason);

// Allocation journal for panic-unwind heap cleanup: see aether_unwind.h.
#include "aether_unwind.h"

// TLS: 1 while executing inside a scheduler-wrapped step, 0 otherwise.
// Signal handlers check this before deciding whether to recover or let
// the signal propagate with SIG_DFL.
extern AETHER_TLS_SHARED int g_aether_in_actor_step;
extern AETHER_TLS_SHARED int g_aether_current_actor_id;

// Ownership of a string returned through a typed fn pointer (#2586). An
// Aether function that hands over owned strings marks the one it returns
// when it is used as a fn-pointer value (aether_fnptr_give in the generated
// prelude); a call through a typed fn pointer clears the mark, and takes its
// result as owned only when the callee marked that very pointer
// (aether_fnptr_take): no C function marks, so a string C returns is
// copied, never freed. The mark is a thread-local of the runtime's, reached
// only through these calls, as current_core_id is (#1751): on Windows a
// thread-local cannot be imported from a DLL, so a program linked against
// the shared runtime could not link when it named the mark itself (#2687).
void aether_fnptr_mark(const void* s);
// Whether r is the marked pointer; clears the mark either way.
int aether_fnptr_claim(const void* r);

// A block of `size` zeroed bytes kept in the calling thread's execution state
// under `key` (the address of something of the caller's own), made at its
// first use and returned again for the same key after; `release`, when not
// NULL, runs on it before it is freed with the thread, or with the task's
// context on a cooperative build with task contexts (docs/design/
// task-execution-contexts.md). NULL when there is no memory. For state that
// a threaded build keeps in a thread-local and a cooperative build must keep
// per task: the builder blocks' stack of generated code, std's last-error
// buffers.
void* aether_context_local(const void* key, size_t size, void (*release)(void*));

#if defined(AETHER_NO_THREADING)
// A cooperative build's stand-in for the thread-local result buffer of a std
// module's split accessors (a pointer from aether_caps_malloc, its capacity
// and its length): a context-local block whose buffer is given back with the
// task's context. Defines NAME_here(), whose ->buf, ->cap and ->len the
// module names as it named its thread-locals; no memory for the block is a
// panic, never another task's buffer. The module includes
// aether_resource_caps.h (aether_caps_free).
#define AETHER_CONTEXT_CAPS_BUFFER(NAME)                                            \
    typedef struct { unsigned char* buf; size_t cap; int len; } NAME##_block_t;      \
    static const char NAME##_key = 0;                                                 \
    static void NAME##_release(void* p) {                                             \
        NAME##_block_t* b = (NAME##_block_t*)p;                                       \
        if (b->buf) aether_caps_free(b->buf, b->cap);                                 \
    }                                                                                 \
    static NAME##_block_t* NAME##_here(void) {                                        \
        NAME##_block_t* b = (NAME##_block_t*)aether_context_local(                    \
            &NAME##_key, sizeof(NAME##_block_t), NAME##_release);                     \
        if (!b) aether_panic("no memory for this task's " #NAME " buffer");           \
        return b;                                                                     \
    }
#endif

/* How the panic and the step barrier reach a copy of the sandbox, which
 * they do not link (#2955, #3393): its unwind of the levels pushed inside a
 * frame a panic leaves, and how a step puts this thread's levels aside and
 * gives them back, each on that copy's own levels. Every copy of the sandbox
 * registers its hooks once, when it first looks for the runtime copies, and
 * the panic and the step barrier serve every copy registered with them:
 * their own, and under ELF interposition several (a library whose code runs
 * the program's panic registers its own sandbox there, beside the program's
 * or another library's). One slot of each, which the last copy to look set,
 * served only that one: a panic caught inside another copy's block left its
 * level in force (#3438). */
typedef struct AetherSandboxHooks {
    void (*unwind)(int frames);
    int (*hide)(int saved[2]);
    void (*show)(const int saved[2]);
} AetherSandboxHooks;

#define AETHER_SANDBOX_HOOKS_MAX 64
extern const AetherSandboxHooks* _Atomic aether_sandbox_hooks[AETHER_SANDBOX_HOOKS_MAX];
// Registers `h`, once: 0, or -1 when every place is taken (a copy past
// AETHER_SANDBOX_HOOKS_MAX refuses every check inside its blocks until a
// place frees: refused more, never less).
int aether_sandbox_hooks_add(const AetherSandboxHooks* h);
// The copies whose levels one step puts aside: those that have some on the
// thread at once, few; one past it keeps its levels in force for the step
// (refused more, never less).
#define AETHER_STEP_SANDBOX_COPIES 8
// A copy that is unloaded gives its place up.
void aether_sandbox_hooks_remove(const AetherSandboxHooks* h);

#ifdef __cplusplus
}
#endif

#endif
