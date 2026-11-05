/* #3394: a chain of linked actors ends in a stack depth of its own, not one
 * that grows with the chain.
 *
 * N actors, each linked to the one before it; none traps exits. Killing the
 * first ends every one: the first's death is told to the second, whose link
 * ends it, and so on down the chain. The schedulers here finalize a death
 * where it is seen (the cooperative one always, the threaded one while it is
 * not running), and the telling ended the next actor from inside the
 * previous one's finalize, so each link of the chain was a few frames more
 * on the stack. The kill runs on a thread with a small stack
 * (run_on_small_stack, stack.c), which a chain that deep overflowed.
 *
 * Built twice by test_link_chain_finalize.sh: against libaether (the
 * threaded scheduler, stopped before the kill) and against the cooperative
 * runtime. */

#include "multicore_scheduler.h"
#include <stdio.h>
#include <stdlib.h>

int run_on_small_stack(void (*fn)(void*), void* arg, size_t bytes);

typedef struct {
    ActorBase** chain;
} Chain;

/* Never stepped: nothing sends to these actors. */
static void chain_step(void* self) {
    (void)self;
}

static void kill_first(void* arg) {
    Chain* c = (Chain*)arg;
    aether_actor_kill(c->chain[0]);
}

int main(int argc, char** argv) {
    int n = argc > 1 ? atoi(argv[1]) : 8000;
    size_t stack = (size_t)(argc > 2 ? atoi(argv[2]) : 256) * 1024;
    scheduler_init(1);
    Chain c;
    c.chain = (ActorBase**)calloc((size_t)n, sizeof(ActorBase*));
    if (!c.chain) return 2;
    for (int i = 0; i < n; i++) {
        c.chain[i] = scheduler_spawn_actor(0, chain_step, sizeof(ActorBase));
        if (!c.chain[i]) {
            printf("spawn %d failed\n", i);
            return 2;
        }
    }
    /* Each one links the one before it, as `actors.link` from its handler. */
    for (int i = 1; i < n; i++) {
        aether_step_actor_set(c.chain[i]);
        aether_actor_link(c.chain[i - 1]);
    }
    aether_step_actor_set(NULL);
#ifndef AETHER_NO_THREADING
    /* Stopped, the threaded scheduler finalizes a death where it is seen, as
     * the cooperative one always does; running, it posts it to the actor's
     * core. */
    scheduler_shutdown();
#endif

    if (run_on_small_stack(kill_first, &c, stack) != 0) {
        printf("no thread\n");
        return 2;
    }

    int ended = 0;
    for (int i = 0; i < n; i++) {
        ActorBase* a = c.chain[i];
        if ((atomic_load(&a->dead) & AETHER_ACTOR_KILLED) &&
            (atomic_load(&a->exit_flags) & AETHER_EXIT_FINALIZED))
            ended++;
    }
    printf("ended %d of %d\n", ended, n);
    free(c.chain);
    return ended == n ? 0 : 1;
}
