// The shared map's tokens (runtime/aether_shared_map.c): a hosted language
// reaches a map only through its token, and revoking the token shuts it out,
// so two live maps must never share a token. They used to come from the
// monotonic clock and a stack address: two maps made back to back, within one
// clock tick, got the same token about half the time on macOS, a lookup found
// the first map, and revoking the second revoked the first.
#include "test_harness.h"
#include "../../runtime/aether_shared_map.h"
#include <stdint.h>

#define MAPS_AT_ONCE 16   /* MAX_ACTIVE_MAPS in aether_shared_map.c */

TEST_CATEGORY(shared_map_back_to_back_tokens_differ, TEST_CATEGORY_RUNTIME) {
    for (int round = 0; round < 2000; round++) {
        uint64_t ta = 0, tb = 0;
        AetherSharedMap* a = aether_shared_map_new(&ta);
        AetherSharedMap* b = aether_shared_map_new(&tb);
        ASSERT_NOT_NULL(a);
        ASSERT_NOT_NULL(b);
        ASSERT_TRUE(ta != 0 && tb != 0);
        ASSERT_TRUE(ta != tb);
        aether_shared_map_put(a, "who", "a");
        aether_shared_map_put(b, "who", "b");
        ASSERT_STREQ("a", aether_shared_map_get_by_token(ta, "who"));
        ASSERT_STREQ("b", aether_shared_map_get_by_token(tb, "who"));
        aether_shared_map_revoke_token(tb);
        ASSERT_NULL(aether_shared_map_get_by_token(tb, "who"));
        ASSERT_STREQ("a", aether_shared_map_get_by_token(ta, "who"));
        aether_shared_map_free(b);
        aether_shared_map_free(a);
    }
}

TEST_CATEGORY(shared_map_tokens_unique_across_registry, TEST_CATEGORY_RUNTIME) {
    AetherSharedMap* maps[MAPS_AT_ONCE];
    uint64_t toks[MAPS_AT_ONCE];
    for (int i = 0; i < MAPS_AT_ONCE; i++) {
        maps[i] = aether_shared_map_new(&toks[i]);
        ASSERT_NOT_NULL(maps[i]);
        for (int j = 0; j < i; j++) ASSERT_TRUE(toks[i] != toks[j]);
    }
    for (int i = 0; i < MAPS_AT_ONCE; i++) aether_shared_map_free(maps[i]);
}
