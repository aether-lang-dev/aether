#include "test_harness.h"
#include "../../std/collections/aether_collections.h"
#include "../../std/string/aether_string.h"

TEST_CATEGORY(list_create_and_free, TEST_CATEGORY_COLLECTIONS) {
    ArrayList* list = list_new();
    ASSERT_NOT_NULL(list);
    ASSERT_EQ(0, list_size(list));
    list_free(list);
}

TEST_CATEGORY(list_add_and_get, TEST_CATEGORY_COLLECTIONS) {
    ArrayList* list = list_new();

    int val1 = 42;
    int val2 = 100;
    list_add_raw(list, &val1);
    list_add_raw(list, &val2);

    ASSERT_EQ(2, list_size(list));
    ASSERT_EQ(&val1, list_get_raw(list, 0));
    ASSERT_EQ(&val2, list_get_raw(list, 1));

    list_free(list);
}

/* Regression: list_get_raw must reject a non-list pointer with a safe NULL
 * rather than faulting on list->size / list->items[index]. A dangling /
 * type-confused pointer (wrong _kind_magic) reaching the accessor used to
 * SIGSEGV deep inside it; the kind + low-address guard now catches it. */
TEST_CATEGORY(list_get_raw_rejects_invalid_pointers, TEST_CATEGORY_COLLECTIONS) {
    /* NULL list. */
    ASSERT_NULL(list_get_raw(NULL, 0));

    /* Low-address scalar: a small int intptr-cast to `ptr`. Below the
     * mmap-min guard, so it must never be dereferenced. */
    ASSERT_NULL(list_get_raw((ArrayList*)(uintptr_t)42, 0));

    /* Another kind mistaken for a list: a HashMap has a valid struct at a
     * valid address but carries AETHER_KIND_MAP_MAGIC, not the list magic —
     * exactly the shape that crashed (valid pointer, wrong magic). */
    HashMap* map = map_new();
    ASSERT_NOT_NULL(map);
    ASSERT_NULL(list_get_raw((ArrayList*)map, 0));
    map_free(map);

    /* The guard must not regress valid lists. */
    ArrayList* list = list_new();
    int v = 7;
    list_add_raw(list, &v);
    ASSERT_EQ(&v, list_get_raw(list, 0));
    list_free(list);
}

TEST_CATEGORY(list_set_and_remove, TEST_CATEGORY_COLLECTIONS) {
    ArrayList* list = list_new();

    int val1 = 1, val2 = 2, val3 = 3;
    list_add_raw(list, &val1);
    list_add_raw(list, &val2);
    list_add_raw(list, &val3);

    int val4 = 99;
    list_set(list, 1, &val4);
    ASSERT_EQ(&val4, list_get_raw(list, 1));

    list_remove(list, 0);
    ASSERT_EQ(2, list_size(list));
    ASSERT_EQ(&val4, list_get_raw(list, 0));

    list_free(list);
}

TEST_CATEGORY(map_create_and_free, TEST_CATEGORY_COLLECTIONS) {
    HashMap* map = map_new();
    ASSERT_NOT_NULL(map);
    ASSERT_EQ(0, map_size(map));
    map_free(map);
}

TEST_CATEGORY(map_put_and_get, TEST_CATEGORY_COLLECTIONS) {
    HashMap* map = map_new();

    int val1 = 42;
    int val2 = 100;

    map_put_raw(map, "name", &val1);
    map_put_raw(map, "age", &val2);

    ASSERT_EQ(2, map_size(map));
    ASSERT_EQ(&val1, map_get_raw(map, "name"));
    ASSERT_EQ(&val2, map_get_raw(map, "age"));

    map_free(map);
}

TEST_CATEGORY(map_has_and_remove, TEST_CATEGORY_COLLECTIONS) {
    HashMap* map = map_new();

    int val = 123;

    map_put_raw(map, "test", &val);
    ASSERT_TRUE(map_has(map, "test"));

    map_remove(map, "test");
    ASSERT_FALSE(map_has(map, "test"));
    ASSERT_EQ(0, map_size(map));

    map_free(map);
}

TEST_CATEGORY(map_keys, TEST_CATEGORY_COLLECTIONS) {
    HashMap* map = map_new();

    int val = 1;
    map_put_raw(map, "a", &val);
    map_put_raw(map, "b", &val);

    MapKeys* keys = map_keys_raw(map);
    ASSERT_NOT_NULL(keys);
    ASSERT_EQ(2, keys->count);

    map_keys_free(keys);
    map_free(map);
}

/* #3831: an owned put hashes and walks its key once. What it stores is what
 * the three walks stored: a new key goes where a plain put puts it (the
 * keys of the two maps come back in the same order across every resize),
 * a replaced value the map owned is released and one it did not own is
 * left alone, a replace moves no key, and freeing the map releases each
 * value it owns, once. */
TEST_CATEGORY(map_owned_put_one_walk_same_map, TEST_CATEGORY_COLLECTIONS) {
    enum { N = 3000 };
    static int plain_value;
    HashMap* plain = map_new();
    HashMap* owned = map_new();
    ASSERT_NOT_NULL(plain);
    ASSERT_NOT_NULL(owned);
    AetherString* first[N];
    char key[32], text[32];
    for (int i = 0; i < N; i++) {
        snprintf(key, sizeof(key), "key-%d", i);
        snprintf(text, sizeof(text), "value-%d", i);
        ASSERT_EQ(1, map_put_raw(plain, key, &plain_value));
        first[i] = string_new(text);
        string_retain(first[i]);          /* ours, to watch the map's go */
        ASSERT_EQ(1, map_put_string_adopted(owned, key, first[i]));
    }
    MapKeys* pk = map_keys_raw(plain);
    MapKeys* ok = map_keys_raw(owned);
    ASSERT_EQ(N, pk->count);
    ASSERT_EQ(N, ok->count);
    int same = 1;
    for (int i = 0; i < N; i++)
        if (strcmp(aether_string_data(map_keys_get_raw(pk, i)),
                   aether_string_data(map_keys_get_raw(ok, i))) != 0) same = 0;
    ASSERT_TRUE(same);

    /* Replace every value: the map's reference to each first value goes. */
    for (int i = 0; i < N; i++) {
        snprintf(key, sizeof(key), "key-%d", i);
        snprintf(text, sizeof(text), "again-%d", i);
        ASSERT_EQ(1, map_put_string_adopted(owned, key, string_new(text)));
    }
    int released = 0;
    for (int i = 0; i < N; i++) released += (first[i]->ref_count & AETHER_STRING_COUNT_MASK) == 1;
    ASSERT_EQ(N, released);
    ASSERT_EQ(N, map_size(owned));
    MapKeys* ok2 = map_keys_raw(owned);
    same = ok2->count == N;
    for (int i = 0; same && i < N; i++)
        if (strcmp(aether_string_data(map_keys_get_raw(ok, i)),
                   aether_string_data(map_keys_get_raw(ok2, i))) != 0) same = 0;
    ASSERT_TRUE(same);
    snprintf(key, sizeof(key), "key-%d", 7);
    ASSERT_STREQ("again-7", aether_string_data(map_get_raw(owned, key)));

    /* A value the map does not own is not released when replaced; one it
     * owns that a plain put replaces is. */
    AetherString* kept = string_new("kept");
    ASSERT_EQ(1, map_put_raw(owned, "key-1", kept));
    ASSERT_EQ(1, map_put_string_adopted(owned, "key-1", string_new("owned")));
    ASSERT_EQ(1, kept->ref_count & AETHER_STRING_COUNT_MASK);
    string_release(kept);
    AetherString* watched = string_new("watched");
    string_retain(watched);
    ASSERT_EQ(1, map_put_string_adopted(owned, "key-2", watched));
    ASSERT_EQ(1, map_put_raw(owned, "key-2", &plain_value));
    ASSERT_EQ(1, watched->ref_count & AETHER_STRING_COUNT_MASK);
    string_release(watched);

    /* A closure box the map owns replaces an owned string, which goes. */
    AetherString* boxed_out = string_new("boxed out");
    string_retain(boxed_out);
    ASSERT_EQ(1, map_put_string_adopted(owned, "key-3", boxed_out));
    ASSERT_EQ(1, map_put_closure_owned(owned, "key-3", NULL));
    ASSERT_EQ(1, boxed_out->ref_count & AETHER_STRING_COUNT_MASK);
    string_release(boxed_out);

    map_keys_free(ok2);
    map_keys_free(ok);
    map_keys_free(pk);
    map_free(owned);
    map_free(plain);
    for (int i = 0; i < N; i++) string_release(first[i]);
}
