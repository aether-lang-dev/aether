// Allocation journal: deterministic heap cleanup on panic unwind.
// See aether_unwind.h for the invariant and the link-surface warning
// that keeps this out of aether_panic.c.

#include "aether_unwind.h"
#include "../utils/aether_compiler.h"
#include <stdlib.h>

typedef struct {
    const void* ptr;
    AetherUnwindFree free_fn;
    int depth;
} AetherUnwindEntry;

typedef struct {
    AetherUnwindEntry* items;
    int count;
    int capacity;
} AetherUnwindJournal;

/* AETHER_TLS_SHARED, not AETHER_TLS: this object is pulled into both
 * executables and --emit=lib shared objects (the emitted prologue
 * references the track/forget symbols from every program). Initial-Exec
 * TLS relocations do not work inside shared objects; Global-Dynamic
 * works in both contexts. Same rationale as aether_panic.c's stack. */
static AETHER_TLS_SHARED AetherUnwindJournal tls_journal = { NULL, 0, 0 };
static AETHER_TLS_SHARED int tls_depth = 0;

void aether_unwind_enter_frame(void) {
    tls_depth++;
}

void aether_unwind_track(const void* p, AetherUnwindFree free_fn) {
    if (tls_depth == 0 || !p || !free_fn) return;
    // Re-tracking an already-journaled pointer updates in place: the
    // ownership handoff shapes (callee returns, caller re-arms its own
    // defer) must never accumulate two entries for one allocation.
    for (int i = tls_journal.count - 1; i >= 0; i--) {
        if (tls_journal.items[i].ptr == p) {
            tls_journal.items[i].free_fn = free_fn;
            tls_journal.items[i].depth = tls_depth;
            return;
        }
    }
    if (tls_journal.count == tls_journal.capacity) {
        int cap = tls_journal.capacity ? tls_journal.capacity * 2 : 32;
        AetherUnwindEntry* grown =
            (AetherUnwindEntry*)realloc(tls_journal.items,
                                        (size_t)cap * sizeof(AetherUnwindEntry));
        if (!grown) return;  // OOM: skip journaling; worst case is the old leak
        tls_journal.items = grown;
        tls_journal.capacity = cap;
    }
    tls_journal.items[tls_journal.count].ptr = p;
    tls_journal.items[tls_journal.count].free_fn = free_fn;
    tls_journal.items[tls_journal.count].depth = tls_depth;
    tls_journal.count++;
}

void aether_unwind_track_if(const void* p, int owned, AetherUnwindFree free_fn) {
    if (owned) aether_unwind_track(p, free_fn);
}

void aether_unwind_forget(const void* p) {
    if (tls_journal.count == 0 || !p) return;
    // Scan newest-first: frees are overwhelmingly LIFO.
    for (int i = tls_journal.count - 1; i >= 0; i--) {
        if (tls_journal.items[i].ptr == p) {
            tls_journal.items[i] = tls_journal.items[tls_journal.count - 1];
            tls_journal.count--;
            return;
        }
    }
}

int aether_unwind_journal_size(void) {
    return tls_journal.count;
}

// Free every entry tagged at the innermost depth and remove it. Runs
// from aether_panic() before the longjmp, while the allocations'
// owning stack frames are still intact (the entries themselves are
// TLS, so intactness only matters for the free functions, which take
// the raw pointer and touch nothing else).
void aether_unwind_drain_current(void) {
    int w = 0;
    for (int i = 0; i < tls_journal.count; i++) {
        if (tls_journal.items[i].depth >= tls_depth) {
            tls_journal.items[i].free_fn(tls_journal.items[i].ptr);
        } else {
            tls_journal.items[w++] = tls_journal.items[i];
        }
    }
    tls_journal.count = w;
}

// Normal frame exit: surviving entries of the popped depth are owned by
// C locals that are still live, so they move under the parent frame's
// protection. With no parent frame nothing can unwind past them; their
// deferred frees run normally, so the entries just leave the journal.
void aether_unwind_exit_frame(void) {
    if (tls_depth <= 1) {
        tls_journal.count = 0;
        tls_depth = 0;
        return;
    }
    for (int i = 0; i < tls_journal.count; i++) {
        if (tls_journal.items[i].depth >= tls_depth) {
            tls_journal.items[i].depth = tls_depth - 1;
        }
    }
    tls_depth--;
}

void aether_unwind_thread_cleanup(void) {
    free(tls_journal.items);
    tls_journal.items = NULL;
    tls_journal.count = 0;
    tls_journal.capacity = 0;
}
