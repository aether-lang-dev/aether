#include "actor_state_machine.h"
#include "../utils/aether_thread.h"
#include "aether_actor_thread.h"
#include "aether_spsc_queue.h"
#include "aether_actor_inbox.h"
#include "../scheduler/multicore_scheduler.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

extern AETHER_TLS int current_core_id;
extern AETHER_TLS int aether_in_actor_thread;   /* multicore_scheduler.c */

// Idle iterations before the thread parks (#2592): as many as a core spins
// before it parks (#1517), so a message that comes soon after the last one
// is taken without a wake-up.
#define AETHER_ACTOR_THREAD_SPINS 10000

int aether_actor_thread_start(pthread_t* thread, void* actor) {
#if AETHER_HAS_THREADS
    return pthread_create(thread, NULL, aether_actor_thread, actor);
#else
    /* A runtime built without threads has none to give it, and a program
     * built with them can still link to it (ci-coop runs ordinary programs
     * on the cooperative runtime this way). The caller then keeps the actor
     * as an ordinary one, stepped by its scheduler. pthread_create is not
     * called here: on a threadless target reaching it is the logic error
     * aether_thread.h describes. */
    (void)thread;
    (void)actor;
    return ENOSYS;
#endif
}

void* aether_actor_thread(void* arg) {
    ActorBase* actor = (ActorBase*)arg;

    // This thread is not its core's scheduler thread, so it sends as every
    // thread the scheduler does not own does: through the foreign channel,
    // under its lock (#3246). With the core's id it wrote straight into the
    // core's mailboxes, its own queue and other cores' queues from that core,
    // whose single producer is the core's thread, and messages were lost or
    // delivered twice.
    current_core_id = -1;
    /* A thread that runs an actor: scheduler_wait() refuses here (#3463). */
    aether_in_actor_thread = 1;

    // The step reads actors from the tables (the ones its handler sends to),
    // so this thread publishes an epoch as a reader (#2517): from here until
    // it exits, refreshed at the top of the loop.
    scheduler_reader_online();

    int idle = 0;
    while (1) {
        // The actor itself is this thread's to run and is not found through
        // a table, so no epoch covers it: a release only marks it, from its
        // own step (which returns here) or elsewhere, and this thread ends
        // it below once it has let go of it. Nothing past the mark may
        // touch it but that.
        if (atomic_load_explicit(&actor->dead, memory_order_acquire)) break;

        // Quiescent point: no actor from a table is held here.
        scheduler_reader_quiescent();

        // Process any pending mailbox messages first to free space.
        if (atomic_load_explicit(&actor->mailbox.count, memory_order_acquire) > 0) {
            idle = 0;
            atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
            if (actor->step) {
                // As a core steps an actor: an ask its handler leaves
                // unanswered fails then, and a panic ends the actor, not
                // the process.
                // A step takes one message, as on a core. The send was
                // counted when it was made, so without the credit
                // scheduler_wait (and with it scheduler_shutdown) waits
                // forever on a message an actor thread handled; and only a
                // step that took one (not a dead actor's) is credited, or the
                // handled count passes the sent count (#3464). Into the
                // count that threads which are no core's share: a core's own
                // is written by its thread alone (#3541).
                int took = scheduler_step_actor(actor);
                if (took) scheduler_count_processed(1);
            }
            continue;
        }

        // Mailbox is empty: move what waits in the inbox into it. Every send
        // to this actor puts its message in the inbox; only this thread
        // touches the mailbox. No more than the empty mailbox holds; the rest
        // waits for the next pass. It used to take up to 128 from a queue of
        // 63 into a mailbox of 32 and drop what did not fit (#2598).
        ActorInbox* inbox = atomic_load_explicit(&actor->inbox, memory_order_acquire);
        Message taken[MAILBOX_SIZE];
        int taken_count = inbox ? actor_inbox_take(inbox, taken, MAILBOX_SIZE) : 0;
        if (taken_count > 0) {
            idle = 0;
            mailbox_send_batch(&actor->mailbox, taken, taken_count);
            continue;
        }

        // An armed receive timeout whose deadline has passed (#3102): the
        // step fires the `after` arm, by the same clock and test as the
        // generated step's. Only a message stepped the actor before, so the
        // timeout of one that received nothing never fired.
        if (actor->timeout_ns > 0 && actor->last_activity_ns > 0 && actor->step &&
            aether_now_ns() - actor->last_activity_ns >= actor->timeout_ns) {
            idle = 0;
            scheduler_countdown_enter();   // until its arm returns (#3443)
            scheduler_step_actor(actor);
            scheduler_countdown_leave();
            continue;
        }

        // Nothing to do: leave once the actor's core has stopped. Checked
        // against every core slot, not just the cores of the running
        // scheduler: scheduler_cleanup() sets num_cores to 0, and a thread
        // that only got here after it would otherwise never see its core's
        // stop.
        int core = atomic_load_explicit(&actor->assigned_core, memory_order_relaxed);
        if (core >= 0 && core < MAX_CORES &&
            !atomic_load_explicit(&schedulers[core].running, memory_order_acquire)) {
            break;
        }

        // Spin a while, as a core does, then sleep until a message, the
        // actor's release or the scheduler's stop wakes the thread (#2592).
        // It used to spin for as long as the actor lived: a whole CPU per
        // idle actor thread, and under Valgrind, which runs one thread at a
        // time, the threads it waited on barely ran (the scheduler tests
        // that start one took most of the hour the memory job took).
        if (idle < AETHER_ACTOR_THREAD_SPINS) {
            idle++;
            AETHER_CPU_PAUSE();
        } else if (!scheduler_actor_thread_park(actor)) {
            AETHER_CPU_PAUSE();   // on no core: nothing could wake it
        }
    }

    // Ended (a kill, a linked death; a panic's is finalized in its step):
    // the death is finalized here, on the thread that owns the mailbox and
    // takes the inbox, while it is still a reader (#2974, #3298).
    scheduler_actor_thread_finalize(actor);

    // Done reading: a released actor this thread stepped can be reclaimed.
    scheduler_reader_offline();
    // And this actor, if it was released (or when it is, later).
    scheduler_actor_thread_exit(actor);

    return NULL;
}
