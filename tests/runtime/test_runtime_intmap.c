// std.intmap (#1986): the C table in std/intmap/aether_intmap.c, held to
// numbers without a compiler in the loop (the valgrind and sanitizer legs run
// this file). The Aether surface end to end is std/intmap/test_intmap.ae.
#include "test_harness.h"
#include "../../std/intmap/aether_intmap.h"
#include <stdint.h>

TEST_CATEGORY(intmap_put_get_remove, TEST_CATEGORY_RUNTIME) {
    AetherIntMap* m = aether_intmap_new(0);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(1, aether_intmap_put(m, 5, 50));
    ASSERT_EQ(0, aether_intmap_put(m, 5, 51));
    ASSERT_EQ(51, (int)aether_intmap_get(m, 5, -1));
    ASSERT_EQ(-1, (int)aether_intmap_get(m, 6, -1));
    ASSERT_EQ(1, aether_intmap_remove(m, 5));
    ASSERT_EQ(0, aether_intmap_remove(m, 5));
    ASSERT_EQ(0, aether_intmap_has(m, 5));
    ASSERT_EQ(1, aether_intmap_put(m, 5, 7));   /* reuses the tombstone */
    ASSERT_EQ(1, aether_intmap_size(m));
    aether_intmap_free(m);
}

TEST_CATEGORY(intmap_grows_and_churns, TEST_CATEGORY_RUNTIME) {
    AetherIntMap* m = aether_intmap_new(4);
    for (int64_t k = 0; k < 50000; k++) ASSERT_EQ(1, aether_intmap_put(m, k << 16, k));
    for (int round = 0; round < 4; round++) {
        for (int64_t k = 0; k < 50000; k += 3) aether_intmap_remove(m, k << 16);
        for (int64_t k = 0; k < 50000; k += 3) aether_intmap_put(m, k << 16, k + round);
    }
    ASSERT_EQ(50000, aether_intmap_size(m));
    ASSERT_EQ(3, (int)aether_intmap_get(m, 0, -1));
    ASSERT_EQ(1, (int)aether_intmap_get(m, (int64_t)1 << 16, -1));
    int seen = 0;
    for (int s = aether_intmap_next(m, 0); s >= 0; s = aether_intmap_next(m, s + 1)) seen++;
    ASSERT_EQ(50000, seen);
    aether_intmap_free(m);
}

TEST_CATEGORY(intmap_add_counts_and_extremes, TEST_CATEGORY_RUNTIME) {
    AetherIntMap* m = aether_intmap_new(0);
    ASSERT_EQ(1, (int)aether_intmap_add(m, INT64_MAX, 1));
    ASSERT_EQ(2, (int)aether_intmap_add(m, INT64_MAX, 1));
    /* A sum that lands on INT64_MIN is a value, not a failure. */
    ASSERT_TRUE(aether_intmap_add(m, INT64_MIN, INT64_MIN) == INT64_MIN);
    ASSERT_EQ(0, aether_intmap_add_failed(m));
    ASSERT_EQ(2, aether_intmap_size(m));
    aether_intmap_clear(m);
    ASSERT_EQ(0, aether_intmap_size(m));
    ASSERT_EQ(-1, aether_intmap_next(m, 0));
    aether_intmap_free(m);
    ASSERT_EQ(0, (int)aether_intmap_add(NULL, 1, 1));
    ASSERT_EQ(-1, aether_intmap_put(NULL, 1, 1));
    ASSERT_EQ(0, aether_intmap_size(NULL));
}
