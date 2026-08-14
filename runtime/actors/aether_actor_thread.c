#include "actor_state_machine.h"
#include "../utils/aether_thread.h"
#include "aether_actor_thread.h"
#include "aether_spsc_queue.h"
#include "../scheduler/multicore_scheduler.h"
#include <stdlib.h>
#include <string.h>

extern AETHER_TLS int current_core_id;

void* aether_actor_thread(void* arg) {
    ActorBase* actor = (ActorBase*)arg;

    // Set TLS core id so that generated send code (scheduler_send_local /
    // scheduler_send_remote) routes same-core messages directly into this
    // actor's SPSC queue, bypassing the incoming_queue → scheduler roundtrip.
    current_core_id = actor->assigned_core;

    while (1) {
        // Process any pending mailbox messages first to free space.
        if (atomic_load_explicit(&actor->mailbox.count, memory_order_acquire) > 0) {
            atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
            if (actor->step) {
                actor->step(arg);
            }
            continue;
        }

        // Mailbox is empty — drain SPSC queue into it.  The scheduler thread
        // (or other actor threads via scheduler_send_local) enqueue here;
        // only this thread touches the mailbox, so no race on head/tail/count.
        Message spsc_msgs[128];
        int spsc_count = actor->spsc_queue
            ? spsc_dequeue_batch(actor->spsc_queue, spsc_msgs, 128) : 0;
        if (spsc_count > 0) {
            mailbox_send_batch(&actor->mailbox, spsc_msgs, spsc_count);
            continue;
        }

        // Nothing to do — check if scheduler is shutting down
        if (actor->assigned_core >= 0 && actor->assigned_core < num_cores) {
            if (!atomic_load_explicit(&schedulers[actor->assigned_core].running, memory_order_acquire)) {
                break;
            }
        }

        // Tight spin — identical to the scheduler thread idle path.
        // Actor threads are expected to each own a core (or share via the
        // scheduler's round-robin on single-core).  Sleeping or yielding
        // here would add latency on every message round-trip.
        AETHER_CPU_PAUSE();
    }

    return NULL;
}
