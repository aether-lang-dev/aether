#include "test_harness.h"
#include "../../runtime/actors/aether_panic.h"
#include "../../std/string/aether_string.h"
#include <string.h>

/* #2340: a heap-built panic message reaches the runtime as the AetherString*
 * codegen passes (so a catch can adopt it), typed const char*. Consumers that
 * want a C string -- the death hook here, the uncaught stderr print covered
 * by tests/integration/panic_uncaught_message -- must unwrap it; reading it as a
 * char* yields the header's magic bytes. */

static char g_hook_reason[128];
static int  g_hook_actor;

static void record_death(int actor_id, const char* reason) {
    g_hook_actor = actor_id;
    strncpy(g_hook_reason, reason, sizeof(g_hook_reason) - 1);
    g_hook_reason[sizeof(g_hook_reason) - 1] = '\0';
}

static void reset_hook(void) {
    g_hook_reason[0] = '\0';
    g_hook_actor = -1;
    aether_set_on_actor_death(record_death);
}

TEST_CATEGORY(death_hook_unwraps_heap_reason, TEST_CATEGORY_RUNTIME) {
    AetherString* msg = string_from_cstr("idx 5 is outside 4");
    ASSERT_NOT_NULL(msg);
    reset_hook();

    aether_fire_death_hook(7, (const char*)msg);
    aether_set_on_actor_death(NULL);
    string_release(msg);

    ASSERT_EQ(7, g_hook_actor);
    ASSERT_STREQ("idx 5 is outside 4", g_hook_reason);
}

TEST_CATEGORY(death_hook_passes_plain_reason_through, TEST_CATEGORY_RUNTIME) {
    reset_hook();

    aether_fire_death_hook(3, "literal reason");
    aether_set_on_actor_death(NULL);

    ASSERT_EQ(3, g_hook_actor);
    ASSERT_STREQ("literal reason", g_hook_reason);
}

TEST_CATEGORY(death_hook_null_reason_is_unknown, TEST_CATEGORY_RUNTIME) {
    reset_hook();

    aether_fire_death_hook(1, NULL);
    aether_set_on_actor_death(NULL);

    ASSERT_EQ(1, g_hook_actor);
    ASSERT_STREQ("unknown", g_hook_reason);
}

/* The caught path must keep handing the catcher the raw pointer it was
 * given: the catch lowering adopts it as the binding's heap string, which is
 * why only the no-frame fallback needed the unwrap. */
TEST_CATEGORY(caught_panic_keeps_raw_heap_reason, TEST_CATEGORY_RUNTIME) {
    AetherString* msg = string_from_cstr("caught heap message");
    ASSERT_NOT_NULL(msg);
    AetherJmpFrame* f = aether_try_push();
    if (AETHER_SIGSETJMP(f->buf, 1) == 0) {
        aether_panic_owned((const char*)msg, string_release);
    }
    bool same_reason = f->reason == (const char*)msg;
    bool same_release = f->reason_release == string_release;
    char text[128];
    snprintf(text, sizeof(text), "%s", f->reason ? aether_string_data(f->reason) : "");
    aether_try_pop();
    // Clean up before assertions: the harness longjmps on a failure.
    string_release(msg);

    ASSERT_TRUE(same_reason);
    ASSERT_TRUE(same_release);
    ASSERT_STREQ("caught heap message", text);
}
