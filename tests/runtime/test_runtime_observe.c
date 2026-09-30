// Observable struct models (#2220): the runtime side table, held to numbers
// without a compiler in the loop. The codegen half (that a field store emits
// the aether_observe_notify_field call) is tests/compiler/test_codegen.c; the two
// together end to end are tests/regression/test_issue2220_observable_struct.ae.
#include "test_harness.h"
#include "../../runtime/aether_observe.h"
#include <stdlib.h>
#include <string.h>

/* A closure environment the way codegen lays one out (#1398): the first field
 * is the destructor the runtime calls to release it. Counting the calls is how
 * these tests see that unobserve released the env exactly once. */
typedef struct {
    void (*dtor)(void*);
    int* freed;
    int* hits;
    void* last_obj;
    void* target;   /* for observers that act on the table from inside a pass */
    long  token;
} Env;

static void env_dtor(void* p) {
    Env* e = (Env*)p;
    (*e->freed)++;
    free(e);
}

static Env* make_env(int* freed, int* hits) {
    Env* e = (Env*)calloc(1, sizeof(Env));
    e->dtor = env_dtor;
    e->freed = freed;
    e->hits = hits;
    return e;
}

static void on_change(void* env, void* obj) {
    Env* e = (Env*)env;
    (*e->hits)++;
    e->last_obj = obj;
}

static AetherObserverClosure closure_of(void (*fn)(void*, void*), Env* e) {
    AetherObserverClosure c;
    c.fn = (void (*)(void))fn;
    c.env = e;
    return c;
}

TEST_CATEGORY(observe_rejects_null_and_empty, TEST_CATEGORY_RUNTIME) {
    int freed = 0, hits = 0;
    int obj = 0;
    AetherObserverClosure none = { NULL, NULL };
    ASSERT_EQ(0, aether_observe(NULL, closure_of(on_change, make_env(&freed, &hits))));
    ASSERT_EQ(1, freed);   /* a refused registration still releases the env */
    ASSERT_EQ(0, aether_observe(&obj, none));
    ASSERT_EQ(0, aether_unobserve(NULL, 1));
    ASSERT_EQ(0, aether_unobserve(&obj, 0));
    ASSERT_EQ(0, aether_unobserve(&obj, 12345));
    ASSERT_EQ(0, aether_unobserve_all(NULL));
    ASSERT_EQ(0, aether_observer_count(NULL));
    ASSERT_EQ(0, aether_observer_count(&obj));
    ASSERT_EQ(0, aether_observe_is_notifying(&obj));
    aether_observe_notify(NULL);          /* no crash */
    aether_observe_notify(&obj);          /* nothing registered: no crash */
    ASSERT_EQ(0, hits);
}

TEST_CATEGORY(observe_notifies_in_registration_order, TEST_CATEGORY_RUNTIME) {
    int freed = 0, hits_a = 0, hits_b = 0;
    int obj = 0;
    Env* a = make_env(&freed, &hits_a);
    Env* b = make_env(&freed, &hits_b);
    long ta = aether_observe(&obj, closure_of(on_change, a));
    long tb = aether_observe(&obj, closure_of(on_change, b));
    ASSERT_TRUE(ta > 0);
    ASSERT_TRUE(tb > ta);
    ASSERT_EQ(2, aether_observer_count(&obj));

    aether_observe_notify(&obj);
    ASSERT_EQ(1, hits_a);
    ASSERT_EQ(1, hits_b);
    ASSERT_TRUE(a->last_obj == &obj);
    ASSERT_TRUE(b->last_obj == &obj);

    aether_observe_notify(&obj);
    ASSERT_EQ(2, hits_a);
    ASSERT_EQ(2, hits_b);

    ASSERT_EQ(1, aether_unobserve(&obj, ta));
    ASSERT_EQ(0, aether_unobserve(&obj, ta));   /* already gone */
    ASSERT_EQ(1, freed);
    ASSERT_EQ(1, aether_observer_count(&obj));
    aether_observe_notify(&obj);
    ASSERT_EQ(2, hits_a);
    ASSERT_EQ(3, hits_b);

    ASSERT_EQ(1, aether_unobserve(&obj, tb));
    ASSERT_EQ(2, freed);
    ASSERT_EQ(0, aether_observer_count(&obj));
    aether_observe_notify(&obj);
    ASSERT_EQ(3, hits_b);
}

TEST_CATEGORY(observe_is_per_object, TEST_CATEGORY_RUNTIME) {
    int freed = 0, hits_x = 0, hits_y = 0;
    int x = 0, y = 0;
    long tx = aether_observe(&x, closure_of(on_change, make_env(&freed, &hits_x)));
    long ty = aether_observe(&y, closure_of(on_change, make_env(&freed, &hits_y)));

    aether_observe_notify(&x);
    ASSERT_EQ(1, hits_x);
    ASSERT_EQ(0, hits_y);

    /* A token belongs to the object it was minted on. */
    ASSERT_EQ(0, aether_unobserve(&y, tx));
    ASSERT_EQ(1, aether_unobserve(&x, tx));
    ASSERT_EQ(1, aether_unobserve(&y, ty));
    ASSERT_EQ(2, freed);
}

TEST_CATEGORY(observe_table_grows_past_initial_capacity, TEST_CATEGORY_RUNTIME) {
    enum { N = 100 };   /* well past the 16-slot first table, through two grows */
    int freed = 0;
    int hits[N];
    int objs[N];
    long tokens[N];
    for (int i = 0; i < N; i++) {
        hits[i] = 0;
        tokens[i] = aether_observe(&objs[i], closure_of(on_change, make_env(&freed, &hits[i])));
        ASSERT_TRUE(tokens[i] > 0);
    }
    for (int i = 0; i < N; i++) aether_observe_notify(&objs[i]);
    for (int i = 0; i < N; i++) {
        ASSERT_EQ(1, hits[i]);
        ASSERT_EQ(1, aether_observer_count(&objs[i]));
    }
    /* Remove every other one, then confirm lookups still land after the
     * tombstones they leave behind. */
    for (int i = 0; i < N; i += 2) ASSERT_EQ(1, aether_unobserve(&objs[i], tokens[i]));
    for (int i = 0; i < N; i++) aether_observe_notify(&objs[i]);
    for (int i = 0; i < N; i++) ASSERT_EQ(i % 2 == 0 ? 1 : 2, hits[i]);
    for (int i = 1; i < N; i += 2) ASSERT_EQ(1, aether_unobserve(&objs[i], tokens[i]));
    ASSERT_EQ(N, freed);
    for (int i = 0; i < N; i++) ASSERT_EQ(0, aether_observer_count(&objs[i]));
}

TEST_CATEGORY(observe_many_observers_on_one_object, TEST_CATEGORY_RUNTIME) {
    int freed = 0, hits = 0;
    int obj = 0;
    for (int i = 0; i < 10; i++) {   /* past the 4-entry first list */
        ASSERT_TRUE(aether_observe(&obj, closure_of(on_change, make_env(&freed, &hits))) > 0);
    }
    ASSERT_EQ(10, aether_observer_count(&obj));
    aether_observe_notify(&obj);
    ASSERT_EQ(10, hits);
    ASSERT_EQ(10, aether_unobserve_all(&obj));
    ASSERT_EQ(10, freed);
    ASSERT_EQ(0, aether_observer_count(&obj));
    ASSERT_EQ(0, aether_unobserve_all(&obj));
}

/* An observer that stores into the object it is being told about: the
 * generated code would call notify again from inside the pass. */
static void reentrant_observer(void* env, void* obj) {
    Env* e = (Env*)env;
    (*e->hits)++;
    ASSERT_EQ(1, aether_observe_is_notifying(obj));
    aether_observe_notify(obj);   /* guarded: no nested pass */
}

TEST_CATEGORY(observe_guards_reentrant_notify, TEST_CATEGORY_RUNTIME) {
    int freed = 0, hits = 0;
    int obj = 0;
    long t = aether_observe(&obj, closure_of(reentrant_observer, make_env(&freed, &hits)));
    ASSERT_EQ(0, aether_observe_is_notifying(&obj));
    aether_observe_notify(&obj);
    ASSERT_EQ(1, hits);
    ASSERT_EQ(0, aether_observe_is_notifying(&obj));
    aether_observe_notify(&obj);
    ASSERT_EQ(2, hits);
    ASSERT_EQ(1, aether_unobserve(&obj, t));
    ASSERT_EQ(1, freed);
}

/* Re-entrancy is per object: an observer of A that stores into B runs B's
 * observers synchronously. */
static void cross_observer(void* env, void* obj) {
    Env* e = (Env*)env;
    (void)obj;
    (*e->hits)++;
    aether_observe_notify(e->target);
}

TEST_CATEGORY(observe_reentrancy_guard_is_per_object, TEST_CATEGORY_RUNTIME) {
    int freed = 0, hits_a = 0, hits_b = 0;
    int a = 0, b = 0;
    Env* ea = make_env(&freed, &hits_a);
    ea->target = &b;
    long ta = aether_observe(&a, closure_of(cross_observer, ea));
    long tb = aether_observe(&b, closure_of(on_change, make_env(&freed, &hits_b)));
    aether_observe_notify(&a);
    ASSERT_EQ(1, hits_a);
    ASSERT_EQ(1, hits_b);
    ASSERT_EQ(1, aether_unobserve(&a, ta));
    ASSERT_EQ(1, aether_unobserve(&b, tb));
    ASSERT_EQ(2, freed);
}

/* An observer that removes itself while running: it must not be freed under
 * its own feet, and must not run again. */
static void self_removing_observer(void* env, void* obj) {
    Env* e = (Env*)env;
    (*e->hits)++;
    ASSERT_EQ(1, aether_unobserve(obj, e->token));
    ASSERT_EQ(0, *e->freed);           /* deferred until the pass ends */
    ASSERT_EQ(1, aether_observer_count(obj));   /* only the neighbour is left */
}

TEST_CATEGORY(observe_unobserve_from_inside_observer_is_deferred, TEST_CATEGORY_RUNTIME) {
    int freed = 0, hits = 0, later_hits = 0;
    int obj = 0;
    Env* e = make_env(&freed, &hits);
    e->token = aether_observe(&obj, closure_of(self_removing_observer, e));
    /* A second observer registered after the self-remover still runs in the
     * same pass: removal never skips a neighbour. */
    long t2 = aether_observe(&obj, closure_of(on_change, make_env(&freed, &later_hits)));

    aether_observe_notify(&obj);
    ASSERT_EQ(1, hits);
    ASSERT_EQ(1, later_hits);
    ASSERT_EQ(1, freed);              /* released once the pass finished */
    ASSERT_EQ(1, aether_observer_count(&obj));

    aether_observe_notify(&obj);
    ASSERT_EQ(1, hits);               /* gone */
    ASSERT_EQ(2, later_hits);
    ASSERT_EQ(1, aether_unobserve(&obj, t2));
    ASSERT_EQ(2, freed);
}

/* An observer that removes a LATER observer mid-pass: the later one must not
 * run in this pass even though the pass snapshot still holds it. */
static void remove_neighbour_observer(void* env, void* obj) {
    Env* e = (Env*)env;
    (*e->hits)++;
    aether_unobserve(obj, e->token);   /* e->token is the neighbour's */
}

TEST_CATEGORY(observe_removed_neighbour_does_not_run, TEST_CATEGORY_RUNTIME) {
    int freed = 0, hits_first = 0, hits_second = 0;
    int obj = 0;
    Env* first = make_env(&freed, &hits_first);
    long t1 = aether_observe(&obj, closure_of(remove_neighbour_observer, first));
    long t2 = aether_observe(&obj, closure_of(on_change, make_env(&freed, &hits_second)));
    first->token = t2;

    aether_observe_notify(&obj);
    ASSERT_EQ(1, hits_first);
    ASSERT_EQ(0, hits_second);
    ASSERT_EQ(1, freed);
    ASSERT_EQ(1, aether_observer_count(&obj));
    ASSERT_EQ(1, aether_unobserve(&obj, t1));
    ASSERT_EQ(2, freed);
}

/* unobserve_all from inside a pass: everything is retired, envs are released
 * once the pass ends, and the slot is gone afterwards. */
static void clear_all_observer(void* env, void* obj) {
    Env* e = (Env*)env;
    (*e->hits)++;
    ASSERT_EQ(2, aether_unobserve_all(obj));
    ASSERT_EQ(0, *e->freed);
}

TEST_CATEGORY(observe_unobserve_all_from_inside_observer, TEST_CATEGORY_RUNTIME) {
    int freed = 0, hits = 0, other_hits = 0;
    int obj = 0;
    ASSERT_TRUE(aether_observe(&obj, closure_of(clear_all_observer, make_env(&freed, &hits))) > 0);
    ASSERT_TRUE(aether_observe(&obj, closure_of(on_change, make_env(&freed, &other_hits))) > 0);
    aether_observe_notify(&obj);
    ASSERT_EQ(1, hits);
    ASSERT_EQ(0, other_hits);
    ASSERT_EQ(2, freed);
    ASSERT_EQ(0, aether_observer_count(&obj));
    ASSERT_EQ(0, aether_observe_is_notifying(&obj));
    aether_observe_notify(&obj);
    ASSERT_EQ(1, hits);
}

/* An observer added during a pass does not run in that pass (the snapshot
 * was taken before), and does run in the next. */
static void adding_observer(void* env, void* obj) {
    Env* e = (Env*)env;
    (*e->hits)++;
    if (e->token == 0) {
        Env* added = make_env(e->freed, (int*)e->target);
        e->token = aether_observe(obj, closure_of(on_change, added));
    }
}

TEST_CATEGORY(observe_added_during_pass_runs_next_pass, TEST_CATEGORY_RUNTIME) {
    int freed = 0, hits = 0, added_hits = 0;
    int obj = 0;
    Env* e = make_env(&freed, &hits);
    e->target = &added_hits;
    long t = aether_observe(&obj, closure_of(adding_observer, e));
    aether_observe_notify(&obj);
    ASSERT_EQ(1, hits);
    ASSERT_EQ(0, added_hits);
    ASSERT_EQ(2, aether_observer_count(&obj));
    aether_observe_notify(&obj);
    ASSERT_EQ(2, hits);
    ASSERT_EQ(1, added_hits);
    long added_token = e->token;
    ASSERT_EQ(1, aether_unobserve(&obj, t));   /* releases e */
    ASSERT_EQ(1, aether_unobserve(&obj, added_token));
    ASSERT_EQ(2, freed);
    ASSERT_EQ(0, aether_observer_count(&obj));
}

/* #2299: an observer registered with aether_observe_fields is called with the
 * field the notification names; aether_observe_notify names none
 * (AETHER_OBSERVE_ANY_FIELD). A plain observer on the same object runs beside
 * it with the object alone. */
static void on_field(void* env, void* obj, int field) {
    Env* e = (Env*)env;
    (*e->hits)++;
    e->last_obj = obj;
    e->token = field;
}

TEST_CATEGORY(observe_fields_receives_the_field, TEST_CATEGORY_RUNTIME) {
    int freed = 0, field_hits = 0, plain_hits = 0;
    int obj = 0;
    Env* f = make_env(&freed, &field_hits);
    Env* p = make_env(&freed, &plain_hits);
    AetherObserverClosure fc;
    fc.fn = (void (*)(void))on_field;
    fc.env = f;
    long tf = aether_observe_fields(&obj, fc);
    long tp = aether_observe(&obj, closure_of(on_change, p));
    ASSERT_TRUE(tf > 0);
    ASSERT_TRUE(tp > tf);

    aether_observe_notify_field(&obj, 3);
    ASSERT_EQ(1, field_hits);
    ASSERT_EQ(1, plain_hits);
    ASSERT_EQ(3, (int)f->token);
    ASSERT_TRUE(f->last_obj == &obj);
    ASSERT_TRUE(p->last_obj == &obj);

    aether_observe_notify(&obj);
    ASSERT_EQ(2, field_hits);
    ASSERT_EQ(2, plain_hits);
    ASSERT_EQ(AETHER_OBSERVE_ANY_FIELD, (int)f->token);

    aether_observe_notify_field(NULL, 0);   /* no crash */
    int spare_hits = 0;
    AetherObserverClosure refused;
    refused.fn = (void (*)(void))on_field;
    refused.env = make_env(&freed, &spare_hits);
    ASSERT_EQ(0, aether_observe_fields(NULL, refused));
    ASSERT_EQ(1, freed);   /* a refused registration still releases the env */

    ASSERT_EQ(1, aether_unobserve(&obj, tf));
    ASSERT_EQ(1, aether_unobserve(&obj, tp));
    ASSERT_EQ(3, freed);
    ASSERT_EQ(0, aether_observer_count(&obj));
    ASSERT_EQ(0, spare_hits);
}
