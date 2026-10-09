#include "actor_state_machine.h"
#include "../utils/aether_thread.h"
#include "aether_actor_thread.h"
#include "aether_spsc_queue.h"
#include "../scheduler/multicore_scheduler.h"
#include <stdlib.h>
#include <string.h>

extern AETHER_TLS int current_core_id;

// Idle iterations before the thread parks (#2592): as many as a core spins
// before it parks (#1517), so a message that comes soon after the last one
// is taken without a wake-up.
#define AETHER_ACTOR_THREAD_SPINS 10000

void* aether_actor_thread(void* arg) {
    ActorBase* actor = (ActorBase*)arg;

    // Set TLS core id so that generated send code (scheduler_send_local /
    // scheduler_send_remote) routes same-core messages directly into this
    // actor's SPSC queue, bypassing the incoming_queue → scheduler roundtrip.
    current_core_id = actor->assigned_core;

    // With that id, a send from this actor's step to another actor of the
    // core steps that actor right here (work inlining) and touches it after
    // its step, so this thread reads like a core and publishes an epoch like
    // one (#2517): from here until it exits, refreshed at the top of the loop.
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
                actor->step(arg);
                // A step takes one message, as on a core. The send was
                // counted when it was made, so without the credit
                // scheduler_wait (and with it scheduler_shutdown) waits
                // forever on a message an actor thread handled.
                int core = actor->assigned_core;
                if (core >= 0 && core < num_cores) {
                    atomic_fetch_add_explicit(&schedulers[core].messages_processed, 1,
                                              memory_order_relaxed);
                }
            }
            continue;
        }

        // Mailbox is empty — drain SPSC queue into it.  The scheduler thread
        // (or other actor threads via scheduler_send_local) enqueue here;
        // only this thread touches the mailbox, so no race on head/tail/count.
        // No more than the empty mailbox holds; the rest stays queued for
        // the next pass. It used to take up to 128 and keep what fit, so a
        // burst of more than MAILBOX_SIZE messages lost the others (#2598).
        Message spsc_msgs[MAILBOX_SIZE];
        int spsc_count = actor->spsc_queue
            ? spsc_dequeue_batch(actor->spsc_queue, spsc_msgs, MAILBOX_SIZE) : 0;
        if (spsc_count > 0) {
            idle = 0;
            mailbox_send_batch(&actor->mailbox, spsc_msgs, spsc_count);
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

    // Done reading: a released actor this thread stepped can be reclaimed.
    scheduler_reader_offline();
    // And this actor, if it was released (or when it is, later).
    scheduler_actor_thread_exit(actor);

    return NULL;
}
