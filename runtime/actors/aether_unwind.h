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
// pulled from libaether.a unconditionally. Keeping the journal here (and
// the per-thread frames block it shares with aether_panic.c, aether_frames.h)
// means programs without try/catch never pull aether_panic.o in. (It used
// to drag the Windows DbgHelp imports, -ldbghelp, with it; it loads
// dbghelp at run time now.) Do not move these definitions into
// aether_panic.c.
//
// All calls are branch-cheap no-ops while no frame is live; the
// scheduler's per-step barrier cost discipline (see aether_panic.h's
// _setjmp note) applies here too.

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*AetherUnwindFree)(const void* p);
void aether_unwind_track(const void* p, AetherUnwindFree free_fn);
void aether_unwind_forget(const void* p);
// #2923: journal a PLACE, the address of a slot that holds a value, with the
// address of the flag that says the slot owns it (NULL: it owns whatever it
// holds). A drain releases what the slot holds then and leaves it empty, so
// the slot's own stores and releases never touch the journal. Forgotten by
// the place's address (aether_unwind_forget) where the slot goes out of scope.
void aether_unwind_track_place(const void* place, const void* owned, AetherUnwindFree release);
// #3406: journal a place of an owned pointer type (`type T = owned ptr drop
// f`): a drain gives what it holds to `drop` and leaves it empty. `drop` is
// the compiler's thunk of the type's drop function (#3413: a drop may return
// a value, or take the pointer typed, as fclose takes a FILE*), which calls
// it as the scope's exit does. Forgotten as any place is. Outside every
// frame (nothing can unwind) it writes nothing. 1 when it journaled the
// place: only then is there anything for the scope exit to forget.
typedef void (*AetherUnwindDropPtr)(void* p);
int aether_unwind_track_owned_place(const void* place, AetherUnwindDropPtr drop);

// Frame lifecycle, called by aether_try_pop / aether_panic and the
// execution frames (aether_exec.c). The journal's depth is the frame depth
// itself (runtime/actors/aether_frames.h), so pushing a frame costs it
// nothing. exit, called with the frame about to go and only while the
// journal holds entries, re-tags the popped depth's surviving entries to
// the parent frame (their owning locals are still live) or empties the
// journal at depth zero; drain_current frees the innermost frame's
// still-live entries (the panic path, just before the longjmp).
void aether_unwind_exit_frame(void);
void aether_unwind_drain_current(void);

// Number of live journal entries on this thread. Exposed for tests.
int aether_unwind_journal_size(void);

// Release the journal's TLS backing storage. Call at thread exit
// (scheduler threads); process exit reclaims the main thread's. Any other
// thread whose journal or teardown queue grew runs it from a thread-exit
// hook (aether_unwind.c, #3539).
void aether_unwind_thread_cleanup(void);

// Tear down an object whose last reference was just released, and whose
// teardown releases others (a closure environment). `obj` starts with its
// teardown function, `void (*)(void*)`. A teardown called while another runs
// on this thread is queued and runs once that one returns, so a chain of
// any length (a closure holding the closure it replaced, 50,000 deep) is
// torn down in constant stack. With no memory to grow the queue, the
// teardown runs at once instead, on the stack.
void aether_teardown(void* obj);

#ifdef __cplusplus
}
#endif

#endif
