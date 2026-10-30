// aether_sandbox.h: runtime containment checks
//
// Each thread has its own stack of grant levels, kept here in the runtime
// (#2955): an enforce block, a host bridge's run_sandboxed call or a
// sandbox_push() adds one, and leaving it takes it off. Stdlib functions
// (fs, net, os, udp) call aether_sandbox_check() before a privileged
// operation: with no level in force on the calling thread everything is
// allowed; with levels in force, the operation must match a grant of every
// one of them (nesting narrows, never widens).
//
// Generated code reaches all of this through the functions below, never by
// naming the per-thread state (a Windows DLL cannot export thread-locals),
// so every library sharing one runtime image (the shared runtime, or, on
// ELF, a library linked without -Bsymbolic, which `ae` no longer makes since
// #3481) shares each thread's levels.
//
// When AETHER_HAS_SANDBOX is not defined, aether_sandbox_check is a
// no-op macro that always returns 1, zero overhead.

#ifndef AETHER_SANDBOX_H
#define AETHER_SANDBOX_H

#include "aether_sandbox_match.h"   /* aether_net_resource, the shared matcher */

#ifdef AETHER_HAS_SANDBOX

#include <stdatomic.h>

// The deepest a thread's levels go: an enforce block, a trusted call's
// barrier and a host bridge's run_sandboxed call each take one. A push past
// it panics, so a level is never silently left out.
#define AETHER_SANDBOX_MAX_LEVELS 64

// Adds a level holding `grants` (a grant list: (category, pattern) pairs;
// borrowed, the pusher keeps it alive until the matching pop). An empty
// list denies everything; NULL allows everything.
void aether_sandbox_push(void* grants);
// Takes the innermost level off.
void aether_sandbox_pop(void);
// This thread's levels, and back to `depth` of them (only ever fewer).
int  aether_sandbox_depth(void);
void aether_sandbox_restore(int depth);

// Trusted names of `sandbox.enforce(perms, name) { ... }` (#1751): the
// enforce call tags the push it is about to make with its site, `image`
// being the address of a static of the generated code that wrote it (site
// numbers are per program or library), and a trusted call pushes a
// barrier that hides the levels from that enforce's up. Returns the depth
// to restore after the call.
void aether_sandbox_tag_site(const void* image, int site);
int  aether_sandbox_trust_enter(const void* image, int site);

// A panic unwinding to the try frame at depth `frames` (aether_try_depth):
// the levels pushed inside that frame are left, as their pops are skipped.
// aether_panic calls this; nothing else needs to.
void aether_sandbox_unwind_to_frame(int frames);

// The check proper, for aether_sandbox_check below: an fs path is resolved
// to where it leads first (".." and symlinks), so a grant for "/box/*"
// cannot be left through "/box/../x" or a symlink in /box; an fs path that
// cannot be resolved is refused.
int aether_sandbox_check_slow(const char* category, const char* resource);

// For the LD_PRELOAD library, which resolves fs paths itself: 1 allowed,
// 0 refused, -1 when no level is in force on this thread (its file-based
// grants then apply).
int aether_sandbox_check_resolved(const char* category, const char* resource);
// The levels in force on this thread, in this runtime image and any other
// it shares the count with (0: none).
int aether_sandbox_levels_here(void);

// A grant pattern as the checks compare it: an fs pattern's directory
// resolved the same way; malloc'd.
char* aether_sandbox_grant_pattern(const char* category, const char* pattern);

// Threads with a level in force, in this runtime image and every other one
// it found in the process (aether_sandbox.c); NULL until the first check or
// push looks for them. Zero is the common case and costs two loads.
extern atomic_int* _Atomic aether_sandbox_in_force;

// Check if an operation is allowed (returns 1 if no level is in force on
// this thread, or if every level in force grants it)
static inline int aether_sandbox_check(const char* category, const char* resource) {
    atomic_int* in_force = atomic_load_explicit(&aether_sandbox_in_force, memory_order_acquire);
    if (in_force && atomic_load_explicit(in_force, memory_order_relaxed) == 0) return 1;
    return aether_sandbox_check_slow(category, resource);
}

#else

// No sandbox compiled in: always allow. The arguments are evaluated for
// their side effects only, so a resource built in a local buffer
// (aether_net_resource) does not leave that buffer unused.
#define aether_sandbox_check(category, resource) ((void)(category), (void)(resource), 1)

#endif // AETHER_HAS_SANDBOX

#endif // AETHER_SANDBOX_H
