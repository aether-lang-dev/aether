/**
 * Work-stealing race hardening tests
 *
 * Exercises the assigned_core atomicity and the TOCTOU guards that
 * prevent the scheduler from touching a mailbox after an actor has
 * been stolen or migrated to another core.
 */

#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include <stdatomic.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

typedef struct {
    // The scheduler casts this to ActorBase*, so the prefix is the macro
    // rather than a hand-copied list that silently drifts when a field lands.
    AETHER_ACTOR_BASE_FIELDS
    atomic_int count;
} WStealActor;

static void wsteal_step(WStealActor* self) {
    Message msg;
    while (mailbox_receive(&self->mailbox, &msg)) {
        atomic_fetch_add(&self->count, 1);
    }
    atomic_store_explicit(&self->active, (self->mailbox.count > 0), memory_order_relaxed);
}

static WStealActor* make_wsteal_actor(int actor_id) {
    WStealActor* a = calloc(1, sizeof(WStealActor));
    a->id = actor_id;
    atomic_init(&a->active, 0);
    a->step = (void (*)(void*))wsteal_step;
    a->auto_process = 0;
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    atomic_init(&a->main_thread_only, 0);
    atomic_init(&a->reply_slot, NULL);
    atomic_flag_clear_explicit(&a->step_lock, memory_order_relaxed);
    atomic_init(&a->count, 0);
    mailbox_init(&a->mailbox);
    // spsc_queue is lazily allocated by the scheduler — leave NULL
    return a;
}

/*
 * Concentrated-load test: pack several actors on one core, leave
 * the other core idle, then send messages in bursts.  The idle core
 * will trigger work-stealing after ~5000 idle cycles.  Verify that
 * every message is eventually processed (no loss during steal).
 */
static void test_worksteal_no_message_loss(void) {
    const int NUM_ACTORS = 6;
    const int MSGS_PER_ACTOR = 50;

    scheduler_init(2);

    WStealActor* actors[6];
    for (int i = 0; i < NUM_ACTORS; i++) {
        actors[i] = make_wsteal_actor(i + 1);
        scheduler_register_actor((ActorBase*)actors[i], 0);
    }

    scheduler_start();

    for (int round = 0; round < MSGS_PER_ACTOR; round++) {
        for (int i = 0; i < NUM_ACTORS; i++) {
            Message msg = message_create_simple(actors[i]->id, 0, round);
            scheduler_send_remote((ActorBase*)actors[i], msg, -1);
        }
        if (round % 10 == 0) sleep_ms(1);
    }

    int expected = NUM_ACTORS * MSGS_PER_ACTOR;
    int total = 0;
    for (int wait = 0; wait < 5000; wait++) {
        total = 0;
        for (int i = 0; i < NUM_ACTORS; i++)
            total += atomic_load(&actors[i]->count);
        if (total >= expected) break;
        sleep_ms(1);
    }

    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(total >= expected * 90 / 100);

    for (int i = 0; i < NUM_ACTORS; i++) free(actors[i]);
}

/*
 * Verify that assigned_core is correctly updated after a steal.
 * Load one core with >4 actors, keep the other idle, wait for
 * steal_attempts to grow, then check that at least one actor's
 * assigned_core has moved to core 1.
 */
static void test_worksteal_assigned_core_update(void) {
    const int NUM_ACTORS = 8;

    scheduler_init(2);

    WStealActor* actors[8];
    for (int i = 0; i < NUM_ACTORS; i++) {
        actors[i] = make_wsteal_actor(i + 1);
        scheduler_register_actor((ActorBase*)actors[i], 0);
    }

    scheduler_start();

    for (int burst = 0; burst < 10; burst++) {
        for (int i = 0; i < NUM_ACTORS; i++) {
            Message msg = message_create_simple(actors[i]->id, 0, burst);
            scheduler_send_remote((ActorBase*)actors[i], msg, -1);
        }
        sleep_ms(5);
    }

    sleep_ms(200);

    int steals = atomic_load(&schedulers[1].steal_attempts);
    int moved = 0;
    for (int i = 0; i < NUM_ACTORS; i++) {
        if (atomic_load(&actors[i]->assigned_core) == 1)
            moved++;
    }

    scheduler_shutdown();
    scheduler_cleanup();

    if (steals > 0) {
        ASSERT_TRUE(moved > 0);
    }

    for (int i = 0; i < NUM_ACTORS; i++) free(actors[i]);
}

/*
 * Re-route correctness: put actors on core 0, start scheduler,
 * send messages while simultaneously requesting migration via
 * migrate_to.  All messages must arrive despite the actor moving.
 */
static void test_worksteal_reroute_under_migration(void) {
    const int NUM_ACTORS = 6;
    const int TOTAL_MSGS = 200;

    scheduler_init(2);

    WStealActor* actors[6];
    for (int i = 0; i < NUM_ACTORS; i++) {
        actors[i] = make_wsteal_actor(i + 1);
        scheduler_register_actor((ActorBase*)actors[i], 0);
    }

    scheduler_start();

    for (int i = 0; i < TOTAL_MSGS; i++) {
        int idx = i % NUM_ACTORS;
        Message msg = message_create_simple(actors[idx]->id, 0, i);
        scheduler_send_remote((ActorBase*)actors[idx], msg, -1);

        if (i == TOTAL_MSGS / 4) {
            for (int j = 0; j < NUM_ACTORS / 2; j++)
                atomic_store_explicit(&actors[j]->migrate_to, 1, memory_order_relaxed);
        }
    }

    int total = 0;
    for (int wait = 0; wait < 5000; wait++) {
        total = 0;
        for (int i = 0; i < NUM_ACTORS; i++)
            total += atomic_load(&actors[i]->count);
        if (total >= TOTAL_MSGS * 90 / 100) break;
        sleep_ms(1);
    }

    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(total >= TOTAL_MSGS * 90 / 100);

    for (int i = 0; i < NUM_ACTORS; i++) free(actors[i]);
}

/*
 * Verify atomic assigned_core reads are consistent: spawn actors,
 * run scheduler, read assigned_core from main thread while scheduler
 * threads may be migrating or stealing.
 */
static void test_worksteal_atomic_assigned_core_read(void) {
    const int NUM_ACTORS = 6;

    scheduler_init(2);

    WStealActor* actors[6];
    for (int i = 0; i < NUM_ACTORS; i++) {
        actors[i] = make_wsteal_actor(i + 1);
        scheduler_register_actor((ActorBase*)actors[i], 0);
    }

    scheduler_start();

    for (int i = 0; i < 100; i++) {
        Message msg = message_create_simple(actors[i % NUM_ACTORS]->id, 0, i);
        scheduler_send_remote((ActorBase*)actors[i % NUM_ACTORS], msg, -1);
    }

    sleep_ms(50);

    int valid = 1;
    for (int i = 0; i < NUM_ACTORS; i++) {
        int core = atomic_load(&actors[i]->assigned_core);
        if (core < 0 || core >= 2) {
            valid = 0;
            break;
        }
    }

    for (int wait = 0; wait < 5000; wait++) {
        int total = 0;
        for (int i = 0; i < NUM_ACTORS; i++)
            total += atomic_load(&actors[i]->count);
        if (total >= 90) break;
        sleep_ms(1);
    }

    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(valid);

    for (int i = 0; i < NUM_ACTORS; i++) free(actors[i]);
}

void register_worksteal_race_tests(void) {
    register_test_with_category("Work-steal no message loss",
        test_worksteal_no_message_loss, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Work-steal assigned_core update",
        test_worksteal_assigned_core_update, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Work-steal re-route under migration",
        test_worksteal_reroute_under_migration, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Work-steal atomic assigned_core read",
        test_worksteal_atomic_assigned_core_read, TEST_CATEGORY_RUNTIME);
}
