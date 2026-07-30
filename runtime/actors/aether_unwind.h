#ifndef AETHER_UNWIND_H
#define AETHER_UNWIND_H

// Allocation journal: deterministic heap cleanup on panic unwind.
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
// Entries carry their free function so this TU never needs to know
// allocation shapes (raw malloc vs refcounted AetherString today;
// typed destructor thunks later). Tracking the same pointer again
// updates the existing entry instead of duplicating it, so a caller
// re-tracking a returned value cannot arm a double free.
//
// LINK-SURFACE WARNING: this journal lives in its OWN translation unit
// on purpose. The emitted C prologue references the track/forget
// symbols from every program, so whatever object provides them is
// pulled from libaether.a unconditionally. aether_panic.o drags the
// Windows DbgHelp stack-trace imports (-ldbghelp) with it; keeping the
// journal here means programs without try/catch never inherit that
// link requirement. Do not move these definitions into aether_panic.c.
//
// All calls are branch-cheap no-ops while no frame is live; the
// scheduler's per-step barrier cost discipline (see aether_panic.h's
// _setjmp note) applies here too.

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*AetherUnwindFree)(const void* p);
void aether_unwind_track(const void* p, AetherUnwindFree free_fn);
void aether_unwind_track_if(const void* p, int owned, AetherUnwindFree free_fn);
void aether_unwind_forget(const void* p);

// Frame lifecycle, called by aether_try_push / aether_try_pop /
// aether_panic. enter increments the journal's own frame depth; exit
// re-tags the popped depth's surviving entries to the parent frame
// (their owning locals are still live) or empties the journal at
// depth zero; drain_current frees the innermost frame's still-live
// entries (the panic path, just before the longjmp).
void aether_unwind_enter_frame(void);
void aether_unwind_exit_frame(void);
void aether_unwind_drain_current(void);

// Number of live journal entries on this thread. Exposed for tests.
int aether_unwind_journal_size(void);

// Release the journal's TLS backing storage. Call at thread exit
// (scheduler threads); process exit reclaims the main thread's.
void aether_unwind_thread_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
