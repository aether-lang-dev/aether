#include "test_harness.h"
#include "../../std/collections/aether_intmap.h"

#include <stdint.h>

/* std.intmap: the integer-keyed hash table (#1986). These exercise the C
 * table directly so the probing, tombstone and growth paths are covered
 * without a compiler in the loop; std/intmap/test_intmap.ae covers the
 * Aether-facing wrappers. */

TEST_CATEGORY(intmap_new_is_empty, TEST_CATEGORY_COLLECTIONS) {
    AetherIntMap* m = aether_intmap_new();
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(0, aether_intmap_size(m));
    ASSERT_FALSE(aether_intmap_has(m, 0));
    ASSERT_EQ(-1, aether_intmap_get_or(m, 0, -1));
    aether_intmap_free(m);
}

TEST_CATEGORY(intmap_put_get_overwrite, TEST_CATEGORY_COLLECTIONS) {
    AetherIntMap* m = aether_intmap_new();
    ASSERT_EQ(1, aether_intmap_put(m, 42, 7));
    ASSERT_EQ(1, aether_intmap_size(m));
    ASSERT_TRUE(aether_intmap_has(m, 42));
    ASSERT_EQ(7, aether_intmap_get_or(m, 42, -1));

    /* Overwrite keeps one entry. */
    ASSERT_EQ(1, aether_intmap_put(m, 42, 8));
    ASSERT_EQ(1, aether_intmap_size(m));
    ASSERT_EQ(8, aether_intmap_get_or(m, 42, -1));

    /* Zero and negative keys and values are ordinary. */
    ASSERT_EQ(1, aether_intmap_put(m, 0, 0));
    ASSERT_EQ(1, aether_intmap_put(m, -1, -5));
    ASSERT_TRUE(aether_intmap_has(m, 0));
    ASSERT_EQ(0, aether_intmap_get_or(m, 0, -1));
    ASSERT_EQ(-5, aether_intmap_get_or(m, -1, 0));
    ASSERT_EQ(3, aether_intmap_size(m));
    aether_intmap_free(m);
}

TEST_CATEGORY(intmap_full_64bit_keys_and_values, TEST_CATEGORY_COLLECTIONS) {
    AetherIntMap* m = aether_intmap_new();
    long long big_key = (long long)INT64_MAX;
    long long small_key = (long long)INT64_MIN;
    ASSERT_EQ(1, aether_intmap_put(m, big_key, small_key));
    ASSERT_EQ(1, aether_intmap_put(m, small_key, big_key));
    ASSERT_EQ(small_key, aether_intmap_get_or(m, big_key, 0));
    ASSERT_EQ(big_key, aether_intmap_get_or(m, small_key, 0));
    /* A key that differs from an existing one only above bit 31 is a
     * different key, not a truncated alias of it. */
    ASSERT_EQ(1, aether_intmap_put(m, 5, 1));
    ASSERT_EQ(1, aether_intmap_put(m, 5LL + (1LL << 40), 2));
    ASSERT_EQ(1, aether_intmap_get_or(m, 5, 0));
    ASSERT_EQ(2, aether_intmap_get_or(m, 5LL + (1LL << 40), 0));
    ASSERT_EQ(4, aether_intmap_size(m));
    aether_intmap_free(m);
}

TEST_CATEGORY(intmap_add_counts_from_zero, TEST_CATEGORY_COLLECTIONS) {
    AetherIntMap* m = aether_intmap_new();
    ASSERT_EQ(1, aether_intmap_add(m, 9, 1));
    ASSERT_EQ(2, aether_intmap_add(m, 9, 1));
    ASSERT_EQ(-3, aether_intmap_add(m, 9, -5));
    ASSERT_EQ(1, aether_intmap_size(m));
    ASSERT_EQ(-3, aether_intmap_get_or(m, 9, 0));
    /* add onto a put value, and a zero delta still inserts the key. */
    aether_intmap_put(m, 10, 100);
    ASSERT_EQ(101, aether_intmap_add(m, 10, 1));
    ASSERT_EQ(0, aether_intmap_add(m, 11, 0));
    ASSERT_TRUE(aether_intmap_has(m, 11));
    ASSERT_EQ(3, aether_intmap_size(m));
    aether_intmap_free(m);
}

TEST_CATEGORY(intmap_remove_then_reinsert, TEST_CATEGORY_COLLECTIONS) {
    AetherIntMap* m = aether_intmap_new();
    aether_intmap_put(m, 1, 10);
    aether_intmap_put(m, 2, 20);
    ASSERT_EQ(1, aether_intmap_remove(m, 1));
    ASSERT_EQ(0, aether_intmap_remove(m, 1));   /* already gone */
    ASSERT_EQ(0, aether_intmap_remove(m, 99));  /* never there */
    ASSERT_EQ(1, aether_intmap_size(m));
    ASSERT_FALSE(aether_intmap_has(m, 1));
    ASSERT_EQ(20, aether_intmap_get_or(m, 2, 0));
    /* The tombstone is reused, and the key reads back fresh. */
    ASSERT_EQ(1, aether_intmap_put(m, 1, 11));
    ASSERT_EQ(11, aether_intmap_get_or(m, 1, 0));
    ASSERT_EQ(2, aether_intmap_size(m));
    aether_intmap_free(m);
}

TEST_CATEGORY(intmap_probe_chain_survives_removal_in_the_middle, TEST_CATEGORY_COLLECTIONS) {
    /* Keys that share a probe start collide into one chain. Removing the
     * first must leave the ones behind it reachable: a remove that emptied
     * the slot instead of tombstoning it would cut the chain. Multiples of
     * the capacity are not enough to force this with a mixing hash, so
     * fill the table densely and check every survivor after each remove. */
    AetherIntMap* m = aether_intmap_new();
    for (long long k = 0; k < 12; k++) aether_intmap_put(m, k, k * 100);
    ASSERT_EQ(16, aether_intmap_capacity(m));  /* 12 <= 3/4 of 16, no growth */
    for (long long gone = 0; gone < 12; gone++) {
        ASSERT_EQ(1, aether_intmap_remove(m, gone));
        for (long long k = gone + 1; k < 12; k++) {
            ASSERT_EQ(k * 100, aether_intmap_get_or(m, k, -1));
        }
        ASSERT_FALSE(aether_intmap_has(m, gone));
    }
    ASSERT_EQ(0, aether_intmap_size(m));
    aether_intmap_free(m);
}

TEST_CATEGORY(intmap_grows_and_keeps_every_entry, TEST_CATEGORY_COLLECTIONS) {
    AetherIntMap* m = aether_intmap_new();
    const long long n = 10000;
    for (long long k = 0; k < n; k++) {
        ASSERT_EQ(1, aether_intmap_put(m, k * 7919, k));
    }
    ASSERT_EQ((int)n, aether_intmap_size(m));
    ASSERT_TRUE(aether_intmap_capacity(m) > 16);
    /* Load factor holds after growth: live entries never exceed 3/4. */
    ASSERT_TRUE((long)aether_intmap_size(m) * 4 <= (long)aether_intmap_capacity(m) * 3);
    for (long long k = 0; k < n; k++) {
        ASSERT_EQ(k, aether_intmap_get_or(m, k * 7919, -1));
    }
    ASSERT_FALSE(aether_intmap_has(m, 1));
    aether_intmap_free(m);
}

TEST_CATEGORY(intmap_churn_does_not_fill_table_with_tombstones, TEST_CATEGORY_COLLECTIONS) {
    /* Insert and remove distinct keys far beyond the capacity while the
     * live count stays tiny. Without tombstone reclamation the table would
     * either grow without bound or, worse, run out of empty slots and
     * probe forever. */
    AetherIntMap* m = aether_intmap_new();
    for (long long k = 0; k < 100000; k++) {
        ASSERT_EQ(1, aether_intmap_put(m, k, k));
        if (k >= 3) ASSERT_EQ(1, aether_intmap_remove(m, k - 3));
    }
    ASSERT_EQ(3, aether_intmap_size(m));
    ASSERT_TRUE(aether_intmap_capacity(m) <= 64);
    ASSERT_EQ(99999, aether_intmap_get_or(m, 99999, -1));
    ASSERT_FALSE(aether_intmap_has(m, 0));
    aether_intmap_free(m);
}

TEST_CATEGORY(intmap_clear_keeps_map_usable, TEST_CATEGORY_COLLECTIONS) {
    AetherIntMap* m = aether_intmap_new();
    for (long long k = 0; k < 50; k++) aether_intmap_put(m, k, k);
    aether_intmap_remove(m, 3);
    aether_intmap_clear(m);
    ASSERT_EQ(0, aether_intmap_size(m));
    ASSERT_FALSE(aether_intmap_has(m, 4));
    ASSERT_EQ(1, aether_intmap_put(m, 4, 44));
    ASSERT_EQ(44, aether_intmap_get_or(m, 4, 0));
    ASSERT_EQ(1, aether_intmap_size(m));
    aether_intmap_free(m);
}

TEST_CATEGORY(intmap_keys_and_values_line_up, TEST_CATEGORY_COLLECTIONS) {
    AetherIntMap* m = aether_intmap_new();
    for (long long k = 1; k <= 5; k++) aether_intmap_put(m, k * 1000, k);
    aether_intmap_remove(m, 3000);
    LongArray* keys = aether_intmap_keys(m);
    LongArray* values = aether_intmap_values(m);
    ASSERT_NOT_NULL(keys);
    ASSERT_NOT_NULL(values);
    ASSERT_EQ(4, longarr_size(keys));
    ASSERT_EQ(4, longarr_size(values));
    long long key_sum = 0, value_sum = 0;
    for (int i = 0; i < 4; i++) {
        long long k = longarr_get_raw(keys, i);
        long long v = longarr_get_raw(values, i);
        ASSERT_EQ(k, v * 1000);               /* same slot order */
        ASSERT_EQ(v, aether_intmap_get_or(m, k, -1));
        key_sum += k;
        value_sum += v;
    }
    ASSERT_EQ(12000, key_sum);
    ASSERT_EQ(12, value_sum);
    longarr_free(keys);
    longarr_free(values);

    /* An empty map snapshots to an empty buffer, not NULL. */
    aether_intmap_clear(m);
    LongArray* none = aether_intmap_keys(m);
    ASSERT_NOT_NULL(none);
    ASSERT_EQ(0, longarr_size(none));
    longarr_free(none);
    aether_intmap_free(m);
}

TEST_CATEGORY(intmap_null_map_is_safe, TEST_CATEGORY_COLLECTIONS) {
    ASSERT_EQ(0, aether_intmap_put(NULL, 1, 1));
    ASSERT_EQ(5, aether_intmap_get_or(NULL, 1, 5));
    ASSERT_FALSE(aether_intmap_has(NULL, 1));
    ASSERT_EQ(0, aether_intmap_add(NULL, 1, 1));
    ASSERT_EQ(0, aether_intmap_remove(NULL, 1));
    ASSERT_EQ(0, aether_intmap_size(NULL));
    ASSERT_EQ(0, aether_intmap_capacity(NULL));
    ASSERT_NULL(aether_intmap_keys(NULL));
    ASSERT_NULL(aether_intmap_values(NULL));
    aether_intmap_clear(NULL);
    aether_intmap_free(NULL);
}
