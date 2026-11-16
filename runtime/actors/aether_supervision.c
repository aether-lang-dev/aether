// Death notices (#2974): monitors, links and the Down message, shared by
// the multicore and the cooperative scheduler.
//
// Every actor that is watched, or watches, has a watch list
// (`ActorBase.death_watch`, made on first use): who monitors it (WATCHER, with the
// monitor id), whom it monitors (MONITORS, the same id), and whom it is
// linked to (LINK). Its death is told once, by aether_watch_notify, which a
// scheduler calls when it finalizes the death on the actor's own thread, or
// a release from the releasing thread: the list is marked finished under its
// lock and its entries taken, each WATCHER is sent a Down, the actor lets go
// of the actors it monitors, and each actor linked to it takes the death: a
// Down if it traps exits, else the same end, unless the reason is "normal".
// A monitor or a link set after that finds the list finished and is answered
// at once, with the reason the death left.
//
// Locks: one watch list at a time, never two. A monitor adds its own
// MONITORS entry first, then its WATCHER entry in the target's list; a
// death or a demonitor that runs between the two finds one side and not the
// other, and each side is checked again where it is used: a Down whose
// MONITORS entry is gone is dropped by the watcher (aether_down_accept), and
// a WATCHER entry whose watcher has let go is removed by that watcher.

#include "../scheduler/multicore_scheduler.h"
#include "../../std/string/aether_string.h"
#include "aether_panic.h"
#include "../aether_resource_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WATCH_WATCHER = 1, WATCH_MONITORS = 2, WATCH_LINK = 3 };

typedef struct {
    int kind;
    int id;              // a monitor's id; 0 for a link
    ActorBase* other;
} AetherWatchEntry;

typedef struct AetherWatch {
    atomic_flag lock;
    int finished;        // the death was told: no entry is added after
    int count;
    int cap;
    AetherWatchEntry* e;
    _Atomic(char*) reason;   // why the actor ended (aether_actor_death_reason)
} AetherWatch;

static atomic_int g_next_monitor_id = 1;

static void watch_lock(AetherWatch* w) {
    while (atomic_flag_test_and_set_explicit(&w->lock, memory_order_acquire)) AETHER_CPU_PAUSE();
}

static void watch_unlock(AetherWatch* w) {
    atomic_flag_clear_explicit(&w->lock, memory_order_release);
}

/* The actor's list, made by the first caller; NULL only out of memory. */
static AetherWatch* watch_of(ActorBase* a) {
    AetherWatch* w = atomic_load_explicit(&a->death_watch, memory_order_acquire);
    if (w) return w;
    AetherWatch* fresh = (AetherWatch*)calloc(1, sizeof(AetherWatch));
    if (!fresh) return NULL;
    atomic_flag_clear_explicit(&fresh->lock, memory_order_relaxed);
    AetherWatch* expected = NULL;
    if (atomic_compare_exchange_strong_explicit(&a->death_watch, &expected, fresh,
                                                memory_order_acq_rel, memory_order_acquire)) {
        return fresh;
    }
    free(fresh);
    return expected;
}

/* Under the list's lock. 0 out of memory. */
static int watch_add(AetherWatch* w, int kind, int id, ActorBase* other) {
    if (w->count == w->cap) {
        int cap = w->cap ? w->cap * 2 : 4;
        AetherWatchEntry* grown = (AetherWatchEntry*)realloc(w->e, (size_t)cap * sizeof(AetherWatchEntry));
        if (!grown) return 0;
        w->e = grown;
        w->cap = cap;
    }
    w->e[w->count].kind = kind;
    w->e[w->count].id = id;
    w->e[w->count].other = other;
    w->count++;
    return 1;
}

/* Under the list's lock: removes the first entry of `kind` with `id` (when
 * `id` is not 0) or `other`; returns it, or kind 0 when there is none. */
static AetherWatchEntry watch_remove(AetherWatch* w, int kind, int id, ActorBase* other) {
    AetherWatchEntry found = { 0, 0, NULL };
    for (int i = 0; i < w->count; i++) {
        AetherWatchEntry* e = &w->e[i];
        if (e->kind != kind) continue;
        if (id ? e->id != id : e->other != other) continue;
        found = *e;
        w->e[i] = w->e[--w->count];
        break;
    }
    return found;
}

/* Removes an entry from `a`'s list, if it has one. */
static void watch_remove_from(ActorBase* a, int kind, int id, ActorBase* other) {
    AetherWatch* w = atomic_load_explicit(&a->death_watch, memory_order_acquire);
    if (!w) return;
    watch_lock(w);
    (void)watch_remove(w, kind, id, other);
    watch_unlock(w);
}

/* A Down for `to`: `dead` ended with `reason`; `id` the monitor's, 0 for a
 * link. The reason is a counted string the message owns. */
static void send_down(ActorBase* to, ActorBase* dead, const char* reason, int id) {
    AetherDownMsg* p = (AetherDownMsg*)malloc(sizeof(AetherDownMsg));
    const char* text = reason ? reason : "normal";
    const char* owned = p ? (const char*)string_new_with_length(text, strlen(text)) : NULL;
    if (!p || !owned) {
        free(p);
        fprintf(stderr, "aether: out of memory for the Down of actor %d to actor %d; "
                        "it is not sent\n", dead->id, to->id);
        return;
    }
    p->_message_id = AETHER_MSG_DOWN;
    p->ref = dead;
    p->reason = owned;
    Message m;
    memset(&m, 0, sizeof m);
    m.type = AETHER_MSG_DOWN;
    m.payload_ptr = p;
    m.payload_int = id;
    scheduler_send_remote(to, m, aether_core_id_get());
}

/* The ends links pass on (#3394). Where a death is finalized as soon as it
 * is seen (the cooperative scheduler, and the threaded one while it is not
 * running), ending an actor tells its own links at once, so a chain of
 * linked actors ended one inside the other: a few frames per actor, and a
 * chain of thousands overflowed a small stack. An end asked for while this
 * thread is making one waits in the thread's queue instead, and the
 * outermost end makes them until none is left: the stack holds one link's
 * frames whatever the chain's length (a chain queues one end at a time).
 * The queue is a stack whose entries each end pushed are turned round, so
 * the ends are made in the order the recursion made them: an actor's links
 * in their list's order, each one's own links before the next. The queue
 * goes when it is empty.
 *
 * A panic can leave the drain. Ending an actor drops what waits for it, and
 * a dropped message runs its release, which is the program's own code for a
 * field of an owned type (its drop function, #3142) and may panic. That
 * panic is caught at the drop and reported through the death hook (#3401),
 * but the hook is a C host's code and runs in the drain too. A jump out of
 * it goes to the nearest frame below (the step barrier of the handler whose
 * kill started the chain, or a `try`), past the drain, and the thread would
 * keep `t_link_ending` set: every later end a link passed on there would be
 * queued and never made. So the outermost end journals the queue with the
 * unwind journal, as a step journals what a panic must release
 * (aether_unwind.h): a panic that leaves it puts the queue back as it was
 * before the end, empty and not ending. The ends still queued are given up
 * with the rest of the work the panic cut short, as the recursion's unwind
 * gave them up. */
typedef struct {
    ActorBase* to;
    char* reason;
} AetherLinkEnd;

static AETHER_TLS AetherLinkEnd* t_link_ends = NULL;
static AETHER_TLS int t_link_end_count = 0;
static AETHER_TLS int t_link_end_cap = 0;
static AETHER_TLS int t_link_ending = 0;
static AETHER_TLS char* t_link_end_reason = NULL;   // the reason of the end being made

/* The queue as it is before an outermost end: empty, nothing held. */
static void link_end_reset(void) {
    for (int i = 0; i < t_link_end_count; i++) free(t_link_ends[i].reason);
    free(t_link_ends);
    t_link_ends = NULL;
    t_link_end_count = 0;
    t_link_end_cap = 0;
    free(t_link_end_reason);
    t_link_end_reason = NULL;
    t_link_ending = 0;
}

/* The journal's release, run by a panic that leaves the drain. */
static void link_end_unwind(const void* p) {
    (void)p;
    link_end_reset();
}

/* 1 when the end of `to` is queued; 0 out of memory. */
static int link_end_queue(ActorBase* to, const char* reason) {
    if (t_link_end_count == t_link_end_cap) {
        int cap = t_link_end_cap ? t_link_end_cap * 2 : 16;
        AetherLinkEnd* q = (AetherLinkEnd*)realloc(t_link_ends, (size_t)cap * sizeof(AetherLinkEnd));
        if (!q) return 0;
        t_link_ends = q;
        t_link_end_cap = cap;
    }
    char* copy = strdup(reason);
    if (!copy) return 0;
    t_link_ends[t_link_end_count].to = to;
    t_link_ends[t_link_end_count].reason = copy;
    t_link_end_count++;
    return 1;
}

/* `to` ended, with what its end queued from `base` on turned round, so the
 * first of them is taken next. */
static void link_end_make(ActorBase* to, const char* reason, int base) {
    scheduler_actor_end(to, reason, AETHER_ACTOR_KILLED);
    for (int lo = base, hi = t_link_end_count - 1; lo < hi; lo++, hi--) {
        AetherLinkEnd t = t_link_ends[lo];
        t_link_ends[lo] = t_link_ends[hi];
        t_link_ends[hi] = t;
    }
}

static void link_end(ActorBase* to, const char* reason) {
    if (t_link_ending) {
        // No memory to queue it: ended here instead, on the stack.
        if (!link_end_queue(to, reason)) scheduler_actor_end(to, reason, AETHER_ACTOR_KILLED);
        return;
    }
    t_link_ending = 1;
    aether_unwind_track(&t_link_ending, link_end_unwind);
    link_end_make(to, reason, 0);
    while (t_link_end_count > 0) {
        AetherLinkEnd e = t_link_ends[--t_link_end_count];
        t_link_end_reason = e.reason;   // the journal's to free, should the end panic
        link_end_make(e.to, e.reason, t_link_end_count);
        t_link_end_reason = NULL;
        free(e.reason);
    }
    aether_unwind_forget(&t_link_ending);
    link_end_reset();
}

/* `dead` ended with `reason`, and `to` was linked to it: `to` gets a Down
 * when it traps exits, else it ends with the same reason, "killed" by it,
 * unless that reason is "normal", which ends nothing. */
static void link_effect(ActorBase* to, ActorBase* dead, const char* reason) {
    const char* r = reason ? reason : "normal";
    if (atomic_load_explicit(&to->exit_flags, memory_order_acquire) & AETHER_EXIT_TRAP) {
        send_down(to, dead, r, 0);
    } else if (strcmp(r, "normal") != 0) {
        link_end(to, r);
    }
}

void aether_actor_claim_reason(ActorBase* a, const char* reason) {
    AetherWatch* w = watch_of(a);
    if (!w) {
        fprintf(stderr, "aether: out of memory keeping why actor %d ended\n", a->id);
        return;
    }
    if (atomic_load_explicit(&w->reason, memory_order_acquire)) return;
    char* copy = strdup(reason ? reason : "normal");
    char* none = NULL;
    if (copy && !atomic_compare_exchange_strong_explicit(&w->reason, &none, copy,
                                                         memory_order_acq_rel,
                                                         memory_order_acquire)) {
        free(copy);
    }
}

const char* aether_actor_death_reason(ActorBase* a) {
    AetherWatch* w = a ? atomic_load_explicit(&a->death_watch, memory_order_acquire) : NULL;
    return w ? atomic_load_explicit(&w->reason, memory_order_acquire) : NULL;
}

int aether_actor_claim_end(ActorBase* a, const char* reason, int bit) {
    if (!(atomic_load_explicit(&a->dead, memory_order_acquire) & AETHER_ACTOR_ENDED))
        aether_actor_claim_reason(a, reason);
    int was = atomic_fetch_or_explicit(&a->dead, bit, memory_order_acq_rel);
    return !(was & AETHER_ACTOR_ENDED);
}

/* The journal's release of the entries a death notice took. */
static void watch_entries_unwind(const void* p) {
    free((void*)p);
}

void aether_watch_notify(ActorBase* a, const char* reason) {
    AetherWatch* w = watch_of(a);
    if (!w) {
        fprintf(stderr, "aether: out of memory telling the death of actor %d\n", a->id);
        return;
    }
    watch_lock(w);
    if (w->finished) {
        watch_unlock(w);
        return;
    }
    w->finished = 1;
    AetherWatchEntry* e = w->e;
    int n = w->count;
    w->e = NULL;
    w->count = 0;
    w->cap = 0;
    watch_unlock(w);

    /* The entries are this call's now, and a link's effect ends actors, so
     * a panic that leaves it skips the free below: journaled until then. */
    if (e) aether_unwind_track(e, watch_entries_unwind);
    for (int i = 0; i < n; i++) {
        switch (e[i].kind) {
        case WATCH_WATCHER:
            send_down(e[i].other, a, reason, e[i].id);
            break;
        case WATCH_MONITORS:
            // It watches no more: its target's death tells it nothing.
            watch_remove_from(e[i].other, WATCH_WATCHER, e[i].id, NULL);
            break;
        case WATCH_LINK:
            // The link goes, both ways, and the other side takes the death.
            watch_remove_from(e[i].other, WATCH_LINK, 0, a);
            link_effect(e[i].other, a, reason);
            break;
        default:
            break;
        }
    }
    if (e) aether_unwind_forget(e);
    free(e);
}

void aether_watch_free(ActorBase* a) {
    AetherWatch* w = atomic_exchange_explicit(&a->death_watch, NULL, memory_order_acq_rel);
    if (w) {
        free(w->e);
        free(atomic_load_explicit(&w->reason, memory_order_acquire));
        free(w);
    }
}

/* A restart in place (#2974): the old incarnation's entries and reason go, the
 * list itself stays. Another actor's monitor or link of the dead child may
 * hold it, to read the death it told under its lock, while the supervisor
 * restarts the child; the new incarnation's death is told through it. */
static void watch_reset(ActorBase* a) {
    AetherWatch* w = atomic_load_explicit(&a->death_watch, memory_order_acquire);
    if (!w) return;
    watch_lock(w);
    AetherWatchEntry* e = w->e;
    char* reason = atomic_exchange_explicit(&w->reason, NULL, memory_order_acq_rel);
    w->e = NULL;
    w->count = 0;
    w->cap = 0;
    w->finished = 0;
    watch_unlock(w);
    free(e);
    free(reason);
}

/* The reason a finished list keeps, copied: read under the list's lock, since
 * a restart may empty it once the lock goes. NULL out of memory. */
static char* watch_reason_copy(AetherWatch* w) {
    const char* r = atomic_load_explicit(&w->reason, memory_order_acquire);
    return strdup(r ? r : "normal");
}

int aether_down_accept(ActorBase* self, Message* msg) {
    /* #3534: a Down in the message's own bytes is one a program sent (the
     * runtime's are heap payloads): payload_int holds its fields there. */
    if (aether_message_is_inline(msg)) return 1;
    int id = (int)msg->payload_int;
    if (id == 0) return 1;   // a link's, or one a program sent
    AetherWatch* w = atomic_load_explicit(&self->death_watch, memory_order_acquire);
    if (!w) return 0;
    watch_lock(w);
    AetherWatchEntry found = watch_remove(w, WATCH_MONITORS, id, NULL);
    watch_unlock(w);
    return found.kind != 0;
}

static void free_reason(const void* p) {
    free((void*)p);
}

/* The actor stepping on this thread, for the std.actors surface. */
static ActorBase* current_self(const char* what) {
    ActorBase* self = (ActorBase*)g_current_step_actor;
    if (!self) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s outside an actor's handler: it acts on the actor running it", what);
        char* owned = strdup(msg);
        if (owned) aether_panic_owned(owned, free_reason);
        aether_panic(what);
    }
    return self;
}

int aether_actor_monitor(void* ref) {
    ActorBase* self = current_self("actors.monitor()");
    ActorBase* target = (ActorBase*)ref;
    if (!target) aether_panic("actors.monitor() of a null actor reference");
    int id = atomic_fetch_add_explicit(&g_next_monitor_id, 1, memory_order_relaxed);
    if (id <= 0) {   // wrapped: ids stay positive (0 marks a link's Down)
        atomic_store_explicit(&g_next_monitor_id, 2, memory_order_relaxed);
        id = 1;
    }
    AetherWatch* mine = watch_of(self);
    if (!mine) aether_panic("actors.monitor(): out of memory");
    watch_lock(mine);
    int ok = watch_add(mine, WATCH_MONITORS, id, target);
    watch_unlock(mine);
    if (!ok) aether_panic("actors.monitor(): out of memory");

    // Released: told at its release; its block may already hold another
    // actor's lists, so it is answered without touching them.
    if (atomic_load_explicit(&target->dead, memory_order_acquire) & AETHER_ACTOR_RELEASED) {
        send_down(self, target, "normal", id);
        return id;
    }
    AetherWatch* theirs = watch_of(target);
    if (!theirs) {
        watch_remove_from(self, WATCH_MONITORS, id, NULL);
        aether_panic("actors.monitor(): out of memory");
    }

    watch_lock(theirs);
    if (theirs->finished) {
        // Ended already: told now, with the reason its death left.
        char* why = watch_reason_copy(theirs);
        watch_unlock(theirs);
        send_down(self, target, why ? why : "normal", id);
        free(why);
        return id;
    }
    ok = watch_add(theirs, WATCH_WATCHER, id, self);
    watch_unlock(theirs);
    if (!ok) {
        watch_remove_from(self, WATCH_MONITORS, id, NULL);
        aether_panic("actors.monitor(): out of memory");
    }
    return id;
}

void aether_actor_demonitor(int id) {
    ActorBase* self = current_self("actors.demonitor()");
    if (id <= 0) return;
    AetherWatch* mine = atomic_load_explicit(&self->death_watch, memory_order_acquire);
    if (!mine) return;
    watch_lock(mine);
    AetherWatchEntry found = watch_remove(mine, WATCH_MONITORS, id, NULL);
    watch_unlock(mine);
    if (found.kind) watch_remove_from(found.other, WATCH_WATCHER, id, NULL);
}

void aether_actor_stop(void) {
    ActorBase* self = current_self("actors.stop()");
    // Finalized when this handler returns (the step barrier's check).
    aether_actor_claim_end(self, "normal", AETHER_ACTOR_STOPPED);
}

void aether_actor_kill(void* ref) {
    if (ref) scheduler_actor_end((ActorBase*)ref, "killed", AETHER_ACTOR_KILLED);
}

int64_t aether_actor_dead_sends(void) {
    return (int64_t)scheduler_dead_sends();
}

// Plain sends to `ref` dropped because its mailbox was full (#2974).
int64_t aether_actor_full_drops(void* ref) {
    return ref ? (int64_t)atomic_load_explicit(&((ActorBase*)ref)->full_drops, memory_order_relaxed) : 0;
}

void aether_actor_link(void* ref) {
    ActorBase* self = current_self("actors.link()");
    ActorBase* other = (ActorBase*)ref;
    if (!other) aether_panic("actors.link() of a null actor reference");
    if (other == self) return;
    AetherWatch* mine = watch_of(self);
    if (!mine) aether_panic("actors.link(): out of memory");
    watch_lock(mine);
    int ok = watch_add(mine, WATCH_LINK, 0, other);
    watch_unlock(mine);
    if (!ok) aether_panic("actors.link(): out of memory");

    // Ended already: the link takes the death now, as it would have then.
    const char* ended_with = NULL;
    char* ended_copy = NULL;
    if (atomic_load_explicit(&other->dead, memory_order_acquire) & AETHER_ACTOR_RELEASED) {
        ended_with = "normal";
    } else {
        AetherWatch* theirs = watch_of(other);
        if (!theirs) {
            watch_remove_from(self, WATCH_LINK, 0, other);
            aether_panic("actors.link(): out of memory");
        }
        watch_lock(theirs);
        if (theirs->finished) {
            ended_copy = watch_reason_copy(theirs);
            ended_with = ended_copy ? ended_copy : "normal";
        } else {
            ok = watch_add(theirs, WATCH_LINK, 0, self);
        }
        watch_unlock(theirs);
        if (!ok) {
            watch_remove_from(self, WATCH_LINK, 0, other);
            aether_panic("actors.link(): out of memory");
        }
    }
    if (ended_with) {
        watch_remove_from(self, WATCH_LINK, 0, other);
        link_effect(self, other, ended_with);
    }
    free(ended_copy);
}

void aether_actor_unlink(void* ref) {
    ActorBase* self = current_self("actors.unlink()");
    ActorBase* other = (ActorBase*)ref;
    if (!other || other == self) return;
    watch_remove_from(self, WATCH_LINK, 0, other);
    if (!(atomic_load_explicit(&other->dead, memory_order_acquire) & AETHER_ACTOR_RELEASED))
        watch_remove_from(other, WATCH_LINK, 0, self);
}

void aether_actor_trap_exits(int on) {
    ActorBase* self = current_self("actors.trap_exits()");
    if (on) atomic_fetch_or_explicit(&self->exit_flags, AETHER_EXIT_TRAP, memory_order_acq_rel);
    else atomic_fetch_and_explicit(&self->exit_flags, ~AETHER_EXIT_TRAP, memory_order_acq_rel);
}

// ---------------------------------------------------------------------------
// Restart in place (#2974)
//
// A supervisor restarts a child under the ref its siblings and clients hold:
// it reruns the child's factory, and the factory's spawn of the child's type
// takes the child's own block. The block is claimed only when no message for
// it can still arrive: the child has ended and its death is finalized (what
// was queued is dropped), its step lock is taken (nothing steps it), and no
// message is counted in flight for it. A counted send reads its target's
// death after counting itself (send_through_core), so one counted after that
// check drops itself while the child is dead, and one that finds it alive
// again reaches the new incarnation: nothing sent before the restart reaches
// it. A local send reads the death on the child's core and drops. The child's
// core touches a dead, quiescent actor through nothing else: it delivers only
// counted messages and steps only under the step lock, which the rebirth
// holds until the new incarnation is alive (scheduler_spawn_ready).
//
// The claim and the reborn block are this thread's (one rebirth at a time),
// never named by generated code.
// ---------------------------------------------------------------------------

static AETHER_TLS int t_capture_depth = 0;   // the spawn capture, below
/* The brackets below this depth are an enclosing step's (#3389): a step
 * nested in it notes no spawn into them and closes none of them. */
static AETHER_TLS int t_capture_floor = 0;
/* Live rebirth claims and spawn captures, on any thread, are counted in
 * aether_caps_slow from bit 8 (AETHER_CAPS_SPAWN_ONE each, aether_resource_caps.h),
 * the word a step reads anyway to know what to put aside. Every spawn passes
 * through aether_rebirth_take and scheduler_spawn_ready; while the count is 0
 * they read no thread-local, which on some targets is a call (emutls). A
 * thread with a claim or a capture of its own sees its own count. A claim
 * counts itself before it reads its child's inflight count, so the
 * scheduler's batch flush, which counts each message in flight first, reads a
 * target's death again only while the count is not 0 (multicore_scheduler.c). */
static AETHER_TLS ActorBase* t_rebirth_target = NULL;
static AETHER_TLS ActorBase* t_reborn = NULL;

int aether_actor_rebirth_begin(void* ref) {
    ActorBase* a = (ActorBase*)ref;
    if (!a || a->auto_process || !a->scheduler_owned) return -1;
    int d = atomic_load_explicit(&a->dead, memory_order_acquire);
    if (!(d & AETHER_ACTOR_ENDED) || (d & AETHER_ACTOR_RELEASED)) return -1;
    if (t_rebirth_target || t_reborn) return -1;
    if (!(atomic_load_explicit(&a->exit_flags, memory_order_acquire) & AETHER_EXIT_FINALIZED))
        return 0;   // its death is still being finalized
    if (atomic_flag_test_and_set_explicit(&a->step_lock, memory_order_acquire)) return 0;
    // Counted before inflight is read, both seq_cst, against a sender that
    // counts its message in inflight and then reads this count (the batch
    // flush): either the claim sees the message in flight, or the sender
    // sees the claim and reads the death again.
    atomic_fetch_add_explicit(&aether_caps_slow, AETHER_CAPS_SPAWN_ONE, memory_order_seq_cst);
    if (atomic_load_explicit(&a->inflight, memory_order_seq_cst) != 0) {
        // Messages sent before its death still on their way: each is
        // dropped where it arrives, and then the child can be reborn.
        atomic_fetch_sub_explicit(&aether_caps_slow, AETHER_CAPS_SPAWN_ONE, memory_order_relaxed);
        atomic_flag_clear_explicit(&a->step_lock, memory_order_release);
        return 0;
    }
    t_rebirth_target = a;
    return 1;
}

/* The block made empty for the state initializers to fill: the old
 * incarnation's state destroyed, its fields zeroed. */
static void rebirth_clear_state(ActorBase* a) {
    if (a->destroy_state) a->destroy_state(a);
    if (a->alloc_size > sizeof(ActorBase))
        memset((char*)a + sizeof(ActorBase), 0, a->alloc_size - sizeof(ActorBase));
}

ActorBase* aether_rebirth_take(void (*step)(void*), size_t size) {
    if (likely(!aether_spawn_watched())) return NULL;
    ActorBase* a = t_rebirth_target;
    if (likely(a == NULL) || a->step != step || a->alloc_size != size) return NULL;
    t_rebirth_target = NULL;
    t_reborn = a;
    rebirth_clear_state(a);
    Message discard;
    while (mailbox_receive(&a->mailbox, &discard)) {   // none: finalized, and nothing
        aether_mailbox_untake(a, discard.type);        // is delivered to a dead actor
        aether_message_drop(a, &discard);
        scheduler_count_dead_sends(1);
    }
    a->step_payload = NULL;
    a->step_type = 0;
    a->timeout_ns = 0;
    a->last_activity_ns = 0;
    atomic_store_explicit(&a->active, 0, memory_order_relaxed);
    atomic_store_explicit(&a->migrate_to, -1, memory_order_relaxed);
    atomic_store_explicit(&a->reply_slot, NULL, memory_order_relaxed);
    watch_reset(a);   // the old incarnation's lists and reason
    atomic_store_explicit(&a->exit_flags, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&a->incarnation, 1, memory_order_relaxed);
    return a;
}

void scheduler_spawn_ready(ActorBase* a) {
    /* A receive timeout the generated spawn armed: its core looks only at
     * the actors it is told of (#3540). A rebirth's core is told again once
     * it is alive (scheduler_actor_revived, below); told now, it finds the
     * actor still dead and leaves it. */
    if (unlikely(a && a->timeout_ns != 0)) scheduler_timeout_armed(a);
    if (likely(!aether_spawn_watched())) return;
    if (unlikely(t_capture_depth > t_capture_floor) && a) aether_actor_capture_note(a);
    if (likely(t_reborn != a) || !a) return;   // a fresh spawn: alive already
    t_reborn = NULL;
    atomic_fetch_sub_explicit(&aether_caps_slow, AETHER_CAPS_SPAWN_ONE, memory_order_relaxed);   // the claim is done
    // Its state is written: the release makes it visible to whoever reads
    // the cleared bits (a sender, the core that steps it).
    atomic_fetch_and_explicit(&a->dead,
                              ~(AETHER_ACTOR_PANICKED | AETHER_ACTOR_KILLED | AETHER_ACTOR_STOPPED),
                              memory_order_acq_rel);
    atomic_flag_clear_explicit(&a->step_lock, memory_order_release);
    scheduler_actor_revived(a);
}

/* A rebirth the factory did not finish: the child stays ended, finalized,
 * with this reason, and its step lock goes. */
static void rebirth_undo(ActorBase* a, int half_made) {
    if (half_made) {
        rebirth_clear_state(a);
        aether_actor_claim_reason(a, "the restart failed");
        atomic_fetch_or_explicit(&a->exit_flags, AETHER_EXIT_FINALIZED, memory_order_acq_rel);
        aether_watch_notify(a, aether_actor_death_reason(a));
    }
    atomic_flag_clear_explicit(&a->step_lock, memory_order_release);
}

int aether_actor_rebirth_end(void* ref) {
    ActorBase* a = (ActorBase*)ref;
    if (!a) return 0;
    if (t_rebirth_target == a) {   // the factory spawned no actor of its type
        t_rebirth_target = NULL;
        rebirth_undo(a, 0);
        atomic_fetch_sub_explicit(&aether_caps_slow, AETHER_CAPS_SPAWN_ONE, memory_order_relaxed);
        return 0;
    }
    if (t_reborn == a) {           // it panicked between the spawn and the ready
        t_reborn = NULL;
        rebirth_undo(a, 1);
        atomic_fetch_sub_explicit(&aether_caps_slow, AETHER_CAPS_SPAWN_ONE, memory_order_relaxed);
        return 0;
    }
    return !(atomic_load_explicit(&a->dead, memory_order_acquire) & AETHER_ACTOR_ENDED);
}

void aether_rebirth_abandon(void) {
    if (likely(!aether_spawn_watched())) return;
    if (likely(!t_rebirth_target && !t_reborn)) return;
    if (t_rebirth_target) rebirth_undo(t_rebirth_target, 0);
    if (t_reborn) rebirth_undo(t_reborn, 1);
    t_rebirth_target = NULL;
    t_reborn = NULL;
    atomic_fetch_sub_explicit(&aether_caps_slow, AETHER_CAPS_SPAWN_ONE, memory_order_relaxed);
}

/* The spawn brackets the running step opened and did not close (a panic
 * skipped their end): closed, down to the step's floor. */
static void capture_close_to_floor(void) {
    while (t_capture_depth > t_capture_floor) {
        t_capture_depth--;
        atomic_fetch_sub_explicit(&aether_caps_slow, AETHER_CAPS_SPAWN_ONE, memory_order_relaxed);
    }
}

/* A step begins inside another (aether_step_barrier.h, #3389). The claim,
 * the reborn block and the brackets this thread holds are the enclosing
 * step's: put aside, so the step's spawns take no claimed block and are no
 * bracket's child, and its panic undoes none of them. 0 when the thread
 * holds none (the caller has seen the spawn count set: some thread
 * does). */
int aether_spawn_put_aside(AetherSpawnAside* s) {
    ActorBase* target = t_rebirth_target;
    ActorBase* reborn = t_reborn;
    int depth = t_capture_depth;
    if (!target && !reborn && depth == t_capture_floor) return 0;
    s->target = target;
    s->reborn = reborn;
    s->capture_floor = t_capture_floor;
    t_rebirth_target = NULL;
    t_reborn = NULL;
    t_capture_floor = depth;
    return 1;
}

/* The step ends, either way. A claim it made and brackets it opened and did
 * not end are undone (they are its own), then the enclosing step's are back. */
void aether_spawn_give_back(const AetherSpawnAside* s) {
    aether_rebirth_abandon();
    capture_close_to_floor();
    t_rebirth_target = s->target;
    t_reborn = s->reborn;
    t_capture_floor = s->capture_floor;
}

/* The step panicked: what it made of its own is undone, the enclosing
 * step's (put aside, when there was any) untouched. */
void aether_spawn_step_panicked(void) {
    if (likely(!aether_spawn_watched())) return;
    aether_rebirth_abandon();
    capture_close_to_floor();
}

int aether_actor_incarnation(void* ref) {
    return ref ? atomic_load_explicit(&((ActorBase*)ref)->incarnation, memory_order_acquire) : 0;
}

// ---------------------------------------------------------------------------
// The child a factory spawned (#2974)
//
// A supervisor's child spec is a closure that spawns the child (and may
// configure it, or spawn helpers first). The supervisor brackets the call:
// the first actor that comes alive on this thread inside the bracket
// (scheduler_spawn_ready, a fresh spawn or a rebirth) is the child.
// Brackets nest, a factory that starts a supervisor of its own included.
// ---------------------------------------------------------------------------

#define AETHER_CAPTURE_DEPTH 16
// t_capture_depth is defined with the rebirth claim, above.
static AETHER_TLS ActorBase* t_captured[AETHER_CAPTURE_DEPTH];

void aether_actor_capture_begin(void) {
    if (t_capture_depth < AETHER_CAPTURE_DEPTH) t_captured[t_capture_depth] = NULL;
    t_capture_depth++;
    atomic_fetch_add_explicit(&aether_caps_slow, AETHER_CAPS_SPAWN_ONE, memory_order_relaxed);
}

void* aether_actor_capture_end(void) {
    if (t_capture_depth <= t_capture_floor) return NULL;   // an enclosing step's
    t_capture_depth--;
    atomic_fetch_sub_explicit(&aether_caps_slow, AETHER_CAPS_SPAWN_ONE, memory_order_relaxed);
    return t_capture_depth < AETHER_CAPTURE_DEPTH ? t_captured[t_capture_depth] : NULL;
}

void aether_actor_capture_note(ActorBase* a) {
    int top = t_capture_depth - 1;
    if (top >= t_capture_floor && top < AETHER_CAPTURE_DEPTH && !t_captured[top]) t_captured[top] = a;
}
