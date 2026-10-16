/* #2527: a send through a reference taken from an actor that has since been
 * released, after its block went to a new actor of the same size. Through
 * the bare pointer that send reaches the new actor; through an
 * AetherActorRef it is dropped and counted. */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "multicore_scheduler.h"
#include "actor_state_machine.h"

#define STALE_TYPE 77

typedef struct {
    ActorBase base;
    atomic_int stale_seen;
    atomic_int fresh_seen;
} Probe;

static void probe_step(void* self) {
    Probe* p = (Probe*)self;
    Message m;
    while (mailbox_receive(&p->base.mailbox, &m)) {
        if (m.type == STALE_TYPE) atomic_fetch_add(&p->stale_seen, 1);
        else atomic_fetch_add(&p->fresh_seen, 1);
    }
}

static Probe* spawn_probe(void) {
    Probe* p = (Probe*)scheduler_spawn_actor(-1, probe_step, sizeof(Probe));
    if (p) {
        atomic_store(&p->stale_seen, 0);
        atomic_store(&p->fresh_seen, 0);
    }
    return p;
}

int actor_ref_probe(void) {
    scheduler_init(2);
    scheduler_start();

    Probe* first = spawn_probe();
    if (!first) return 1;
    AetherActorRef ref = scheduler_actor_ref(&first->base);
    scheduler_release_actor(&first->base);

    /* The released block goes to a later spawn of its size once every core
     * has passed the top of its loop; spawn until one gets it. */
    Probe* reused = NULL;
    Probe* others[4096];
    int n = 0;
    for (int tries = 0; tries < 4096 && !reused; tries++) {
        Probe* p = spawn_probe();
        if (!p) return 2;
        if (p == first) reused = p;
        else others[n++] = p;
        usleep(200);
    }
    if (!reused) {
        printf("FAIL: the released block was never reused\n");
        return 3;
    }

    uint64_t before = scheduler_released_sends();
    Message stale;
    memset(&stale, 0, sizeof(stale));
    stale.type = STALE_TYPE;
    scheduler_send_ref(ref, stale);

    Message fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.type = 1;
    scheduler_send_ref(scheduler_actor_ref(&reused->base), fresh);
    scheduler_wait();

    int ok = 1;
    if (scheduler_released_sends() != before + 1) {
        printf("FAIL: the stale send was not counted\n");
        ok = 0;
    }
    if (atomic_load(&reused->stale_seen) != 0) {
        printf("FAIL: the stale send reached the new actor\n");
        ok = 0;
    }
    if (atomic_load(&reused->fresh_seen) != 1) {
        printf("FAIL: a send through the new actor's reference was not delivered\n");
        ok = 0;
    }
    (void)others;
    scheduler_shutdown();
    if (ok) printf("actor_ref_after_release passed\n");
    return ok ? 0 : 4;
}
