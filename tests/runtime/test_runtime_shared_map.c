// The shared map's tokens (runtime/aether_shared_map.c): a hosted language
// reaches a map only through its token, and revoking the token shuts it out,
// so two live maps must never share a token. They used to come from the
// monotonic clock and a stack address: two maps made back to back, within one
// clock tick, got the same token about half the time on macOS, a lookup found
// the first map, and revoking the second revoked the first.
#include "test_harness.h"
#include "../../runtime/aether_shared_map.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
        ASSERT_EQ(0, aether_shared_map_put(a, "who", 3, "a", 1));
        ASSERT_EQ(0, aether_shared_map_put(b, "who", 3, "b", 1));
        ASSERT_STREQ("a", aether_shared_map_get_by_token(ta, "who", 3, NULL));
        ASSERT_STREQ("b", aether_shared_map_get_by_token(tb, "who", 3, NULL));
        aether_shared_map_revoke_token(tb);
        ASSERT_NULL(aether_shared_map_get_by_token(tb, "who", 3, NULL));
        ASSERT_STREQ("a", aether_shared_map_get_by_token(ta, "who", 3, NULL));
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

// Keys and values carry their length (aether_shared_map.h): a NUL is a byte.
// The C-string map cut "a\0b" to "a" on put, and its block used the NUL as
// its separator.
TEST_CATEGORY(shared_map_nul_in_key_and_value, TEST_CATEGORY_RUNTIME) {
    uint64_t t = 0;
    AetherSharedMap* m = aether_shared_map_new(&t);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(0, aether_shared_map_put(m, "k\0x", 3, "a\0b", 3));
    size_t n = 99;
    const char* v = aether_shared_map_get(m, "k\0x", 3, &n);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(3, (int)n);
    ASSERT_TRUE(memcmp(v, "a\0b", 3) == 0);
    ASSERT_EQ(0, (int)v[3]);                       // NUL-terminated after its length
    ASSERT_NULL(aether_shared_map_get(m, "k", 1, &n));   // the cut key is another key
    ASSERT_EQ(0, (int)n);
    // A guest's put through the token: by length too, and a frozen input
    // is refused by its whole key.
    aether_shared_map_freeze_inputs(m);
    ASSERT_EQ(-1, aether_shared_map_put_by_token(t, "k\0x", 3, "z", 1));
    ASSERT_EQ(0, aether_shared_map_put_by_token(t, "k", 1, "y\0", 2));
    v = aether_shared_map_get_by_token(t, "k", 1, &n);
    ASSERT_EQ(2, (int)n);
    size_t kl = 0;
    ASSERT_TRUE(memcmp(aether_shared_map_key_at(m, 0, &kl), "k\0x", 3) == 0);
    ASSERT_EQ(3, (int)kl);
    ASSERT_EQ(2, (int)aether_shared_map_value_len_at(m, 1));
    ASSERT_EQ(-1, (int)aether_shared_map_value_len_at(m, 2));
    // NULL value: an empty one.
    ASSERT_EQ(0, aether_shared_map_put(m, "nil", 3, NULL, 0));
    v = aether_shared_map_get(m, "nil", 3, &n);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(0, (int)n);
    aether_shared_map_free(m);
}

TEST_CATEGORY(shared_map_full_refuses, TEST_CATEGORY_RUNTIME) {
    AetherSharedMap* m = aether_shared_map_new(NULL);
    char k[16];
    for (int i = 0; i < AETHER_SHARED_MAP_MAX_ENTRIES; i++) {
        int kn = snprintf(k, sizeof k, "k%d", i);
        ASSERT_EQ(0, aether_shared_map_put(m, k, (size_t)kn, "v", 1));
    }
    ASSERT_EQ(-1, aether_shared_map_put(m, "more", 4, "v", 1));
    ASSERT_EQ(AETHER_SHARED_MAP_MAX_ENTRIES, aether_shared_map_count(m));
    ASSERT_EQ(0, aether_shared_map_put(m, "k0", 2, "w", 1));   // an update still works
    aether_shared_map_free(m);
}

static uint32_t u32_at(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Layout 2: magic, a zero word, the version, frozen_count, entry_count, then
// length-prefixed entries; read back whole, NULs and all.
TEST_CATEGORY(shared_map_block_layout_2, TEST_CATEGORY_RUNTIME) {
    AetherSharedMap* m = aether_shared_map_new(NULL);
    ASSERT_EQ(0, aether_shared_map_put(m, "in", 2, "x\0y", 3));
    aether_shared_map_freeze_inputs(m);
    ASSERT_EQ(0, aether_shared_map_put(m, "o\0ut", 4, "", 0));
    size_t n = 0;
    unsigned char* b = aether_shared_map_encode(m, &n);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ(20 + (8 + 2 + 3) + (8 + 4 + 0), (int)n);
    ASSERT_TRUE(memcmp(b, "AESM", 4) == 0);
    ASSERT_EQ(0, (int)u32_at(b + 4));
    ASSERT_EQ(2, (int)u32_at(b + 8));
    ASSERT_EQ(1, (int)u32_at(b + 12));
    ASSERT_EQ(2, (int)u32_at(b + 16));
    ASSERT_EQ(2, (int)u32_at(b + 20));
    ASSERT_EQ(3, (int)u32_at(b + 24));
    ASSERT_TRUE(memcmp(b + 28, "inx\0y", 5) == 0);
    // A layout-1 reader stops at byte 4 (a NUL) and sees an empty map.
    ASSERT_EQ(0, (int)b[4]);

    // Read back into a host map whose own input "in" is frozen: the
    // block's other entry is an output and is taken whole; the input is
    // not overwritten even when the block (a child's) changes it.
    AetherSharedMap* host = aether_shared_map_new(NULL);
    ASSERT_EQ(0, aether_shared_map_put(host, "in", 2, "host", 4));
    aether_shared_map_freeze_inputs(host);
    ASSERT_EQ(0, aether_shared_map_decode_outputs(host, b, n));
    size_t vl = 0;
    ASSERT_STREQ("host", aether_shared_map_get(host, "in", 2, &vl));
    ASSERT_NOT_NULL(aether_shared_map_get(host, "o\0ut", 4, &vl));
    ASSERT_EQ(0, (int)vl);

    // Refused whole, the map unchanged: the old layout, a wrong magic, a
    // wrong version, a nonzero second word, a length past the end, trailing
    // bytes, a short header.
    int before = aether_shared_map_count(host);
    unsigned char old1[] = { 1, 0, 0, 0, 'k', 0, 'v', 0, 'o', 0, 'w', 0, 0 };
    ASSERT_EQ(-1, aether_shared_map_decode_outputs(host, old1, sizeof old1));
    unsigned char* bad = malloc(n + 1);
    memcpy(bad, b, n); bad[0] = 'X';
    ASSERT_EQ(-1, aether_shared_map_decode_outputs(host, bad, n));
    memcpy(bad, b, n); bad[8] = 3;
    ASSERT_EQ(-1, aether_shared_map_decode_outputs(host, bad, n));
    memcpy(bad, b, n); bad[5] = 1;
    ASSERT_EQ(-1, aether_shared_map_decode_outputs(host, bad, n));
    memcpy(bad, b, n); bad[24] = 0xff; bad[25] = 0xff;   // value_len past the end
    ASSERT_EQ(-1, aether_shared_map_decode_outputs(host, bad, n));
    memcpy(bad, b, n); bad[n] = 7;                       // a trailing byte that is not padding
    ASSERT_EQ(-1, aether_shared_map_decode_outputs(host, bad, n + 1));
    ASSERT_EQ(-1, aether_shared_map_decode_outputs(host, b, 19));
    ASSERT_EQ(before, aether_shared_map_count(host));
    memcpy(bad, b, n); bad[n] = 0;                       // zero padding (a page-rounded shm) is read
    ASSERT_EQ(0, aether_shared_map_decode_outputs(host, bad, n + 1));
    free(bad);
    free(b);
    aether_shared_map_free(host);
    aether_shared_map_free(m);
}

// The block is capped at 64 KiB, as layout 1's buffer was, but refused
// loudly: layout 1 wrote past its 64 KiB stack buffer for a large value.
TEST_CATEGORY(shared_map_block_too_large, TEST_CATEGORY_RUNTIME) {
    AetherSharedMap* m = aether_shared_map_new(NULL);
    size_t big = AETHER_SHARED_MAP_SHM_MAX_WRITE;   // with its header, over the cap
    char* v = malloc(big);
    memset(v, 'x', big);
    ASSERT_EQ(0, aether_shared_map_put(m, "big", 3, v, big));
    size_t n = 1;
    errno = 0;
    ASSERT_NULL(aether_shared_map_encode(m, &n));
    ASSERT_EQ(E2BIG, errno);
    ASSERT_EQ(0, (int)n);
    errno = 0;
    ASSERT_NULL(aether_shared_map_to_shm(m));
    ASSERT_TRUE(errno == E2BIG || errno == ENOSYS);
    free(v);
    aether_shared_map_free(m);
}

#if (defined(__linux__) && !defined(__ANDROID__)) || defined(__APPLE__)
// Through a real shm block: what a child adds comes back, NULs and all.
TEST_CATEGORY(shared_map_shm_round_trip, TEST_CATEGORY_RUNTIME) {
    uint64_t t = 0;
    AetherSharedMap* m = aether_shared_map_new(&t);
    ASSERT_EQ(0, aether_shared_map_put(m, "in", 2, "a\0b", 3));
    aether_shared_map_freeze_inputs(m);
    char* name = aether_shared_map_to_shm_by_token(t);
    ASSERT_NOT_NULL(name);
    // The child: reads the block, adds an output, writes it back.
    AetherSharedMap* child = aether_shared_map_new(NULL);
    ASSERT_EQ(0, aether_shared_map_read_outputs_from_shm(child, name));
    size_t vl = 0;
    const char* got = aether_shared_map_get(child, "in", 2, &vl);
    ASSERT_NOT_NULL(got);
    ASSERT_EQ(3, (int)vl);
    ASSERT_EQ(0, aether_shared_map_put(child, "out\0", 4, "c\0d", 3));
    char* cname = aether_shared_map_to_shm(child);
    ASSERT_NOT_NULL(cname);
    ASSERT_EQ(0, aether_shared_map_read_outputs_from_shm_by_token(t, cname));
    got = aether_shared_map_get(m, "out\0", 4, &vl);
    ASSERT_NOT_NULL(got);
    ASSERT_EQ(3, (int)vl);
    ASSERT_TRUE(memcmp(got, "c\0d", 3) == 0);
    ASSERT_EQ(-1, aether_shared_map_read_outputs_from_shm(m, "/aether_map_no_such_block"));
    aether_shared_map_unlink_shm(cname);
    aether_shared_map_unlink_shm(name);
    free(cname);
    free(name);
    aether_shared_map_free(child);
    aether_shared_map_free(m);
}
#endif
