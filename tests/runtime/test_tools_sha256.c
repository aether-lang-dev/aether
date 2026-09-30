// SHA-256 in `ae` (#2322): tools/ae_sha256.c against the FIPS 180-4 / NIST
// example vectors, fed whole and in pieces that straddle block and padding
// boundaries, and over a file. `ae add` verifies release artifacts with it.
#include "test_harness.h"
#include "../../tools/ae_sha256.h"
#include <stdio.h>
#include <string.h>

static void hex_of(const unsigned char d[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = digits[d[i] >> 4];
        out[2 * i + 1] = digits[d[i] & 15];
    }
    out[64] = '\0';
}

static void digest_in_steps(const char* msg, size_t step, char out[65]) {
    AeSha256 c;
    ae_sha256_init(&c);
    size_t len = strlen(msg);
    for (size_t i = 0; i < len; i += step) {
        size_t take = len - i < step ? len - i : step;
        ae_sha256_update(&c, msg + i, take);
    }
    unsigned char d[32];
    ae_sha256_final(&c, d);
    hex_of(d, out);
}

TEST_CATEGORY(tools_sha256_nist_vectors, TEST_CATEGORY_RUNTIME) {
    char h[65];
    digest_in_steps("", 1, h);
    ASSERT_STREQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", h);
    digest_in_steps("abc", 3, h);
    ASSERT_STREQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", h);
    /* 56 bytes: the length no longer fits the first block's tail. */
    const char* two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    digest_in_steps(two_block, 56, h);
    ASSERT_STREQ("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", h);
    /* The same message fed a byte at a time, and seven at a time. */
    digest_in_steps(two_block, 1, h);
    ASSERT_STREQ("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", h);
    digest_in_steps(two_block, 7, h);
    ASSERT_STREQ("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", h);
}

TEST_CATEGORY(tools_sha256_million_a, TEST_CATEGORY_RUNTIME) {
    AeSha256 c;
    ae_sha256_init(&c);
    char chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    for (int i = 0; i < 1000; i++) ae_sha256_update(&c, chunk, sizeof(chunk));
    unsigned char d[32];
    char h[65];
    ae_sha256_final(&c, d);
    hex_of(d, h);
    ASSERT_STREQ("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", h);
}

TEST_CATEGORY(tools_sha256_file, TEST_CATEGORY_RUNTIME) {
    char path[] = "test_tools_sha256.tmp";
    FILE* f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fputs("abc", f);
    fclose(f);
    char h[65];
    ASSERT_EQ(0, ae_sha256_file_hex(path, h));
    ASSERT_STREQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", h);
    remove(path);
    ASSERT_EQ(-1, ae_sha256_file_hex("no/such/file.bin", h));
}
