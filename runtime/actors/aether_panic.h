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
//   - Windows / Emscripten / freestanding: plain setjmp/longjmp.
//     Win32 has no POSIX signal semantics so setjmp is already fast;
//     wasm and bare-metal have no sigjmp_buf at all.
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
#if defined(_WIN32) || defined(__EMSCRIPTEN__) || (defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 0)
  #define AETHER_SIGSETJMP(buf, savemask) setjmp(buf)
  #define AETHER_SIGLONGJMP(buf, val)     longjmp((buf), (val))
#else
  #define AETHER_SIGSETJMP(buf, savemask) _setjmp(buf)
  #define AETHER_SIGLONGJMP(buf, val)     _longjmp((buf), (val))
#endif

// Maximum nesting depth of try/catch (and scheduler step barriers) per
// thread. 32 covers any realistic handler; the limit lets the stack live
// in TLS without dynamic allocation.
#define AETHER_PANIC_MAX_DEPTH 32

typedef struct AetherJmpFrame {
    aether_sigjmp_buf buf;
    const char* reason;   // written by aether_panic() just before siglongjmp
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
// step() unwinds. NULL clears. Single global slot — if you need fan-out,
// dispatch yourself.
typedef void (*AetherDeathHook)(int actor_id, const char* reason);
void aether_set_on_actor_death(AetherDeathHook fn);

// Called by the scheduler after catching a panic. Fires the on_actor_death
// hook if one is set. Separated from the push/pop primitives so scheduler
// code can decide its own ordering (e.g. mark actor dead first, then fire
// the hook).
void aether_fire_death_hook(int actor_id, const char* reason);

// ---------------------------------------------------------------------------
// Allocation journal (#1301): deterministic heap cleanup on unwind.
//
// While at least one panic frame is live, generated code journals every
// heap allocation whose deferred scope-exit free is armed, and forgets
// the entry when that free runs or when ownership escapes (return,
// container/actor adoption). aether_panic() drains the innermost
// frame's still-live entries before longjmp, freeing exactly the
// deferred frees the jump would have skipped.
//
// INVARIANT the codegen emission must preserve: journal contents ==
// the set of armed-but-unfired deferred frees. Track without a
// matching forget on every ownership-transfer path turns the drain
// into a use-after-free; forget without track is always safe (the
// entry just is not there). When in doubt, forget.
//
// Entries carry their free function so the runtime never needs to know
// allocation shapes (raw malloc vs refcounted AetherString today;
// typed destructor thunks later). Tracking the same pointer again
// updates the existing entry instead of duplicating it, so a
// caller re-tracking a returned value cannot arm a double free.
//
// All three calls are branch-cheap no-ops while no frame is live; the
// per-step scheduler barrier's cost discipline (see the _setjmp note
// above) applies here too.
// ---------------------------------------------------------------------------
typedef void (*AetherUnwindFree)(const void* p);
void aether_unwind_track(const void* p, AetherUnwindFree free_fn);
void aether_unwind_track_if(const void* p, int owned, AetherUnwindFree free_fn);
void aether_unwind_forget(const void* p);
// Number of live journal entries on this thread. Exposed for tests.
int aether_unwind_journal_size(void);
// Release the journal's TLS backing storage. Call at thread exit
// (scheduler threads); process exit reclaims the main thread's.
void aether_unwind_thread_cleanup(void);

// TLS: 1 while executing inside a scheduler-wrapped step, 0 otherwise.
// Signal handlers check this before deciding whether to recover or let
// the signal propagate with SIG_DFL.
extern AETHER_TLS_SHARED int g_aether_in_actor_step;
extern AETHER_TLS_SHARED int g_aether_current_actor_id;

#ifdef __cplusplus
}
#endif

#endif
