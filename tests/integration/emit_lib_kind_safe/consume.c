/*
 * consume.c — Kind-discriminator + safe deep-free integration test.
 *
 * Asserts:
 *   1. aether_value_kind(NULL) == UNKNOWN
 *   2. aether_value_kind((AetherValue*)42) == UNKNOWN
 *      (low-address guard: 42 is in the unmapped zero page;
 *      probe must NOT segfault and MUST report UNKNOWN)
 *   3. aether_value_kind(root) == MAP for the script-built map
 *   4. aether_value_is_map(root) == 1
 *   5. aether_value_is_list(root) == 0
 *   6. aether_value_kind(get_map(root, "addr")) == MAP
 *   7. aether_value_kind(get_list(root, "tags")) == LIST
 *   8. aether_value_kind on the int slot ("age") returns UNKNOWN
 *      (the stored void* is (intptr_t)42 — schema mismatch into
 *      kind probe must not crash)
 *   9. aether_config_free_deep(root) returns; subsequent
 *      aether_value_kind(root) reports UNKNOWN (UAF magic cleared)
 *
 * The whole point of this test is that EVERY one of these
 * assertions runs without segfaulting — that's the safety
 * contract. If the low-address guard or magic check fail, the
 * process crashes and the test fails loudly.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* The library is loaded by path on every platform: dlopen on POSIX,
 * LoadLibrary on Windows (#2541). */
#ifdef _WIN32
#include <windows.h>
static void* lib_open(const char* path) { return (void*)LoadLibraryA(path); }
static void* lib_sym(void* h, const char* name) {
    return (void*)GetProcAddress((HMODULE)h, name);
}
static void lib_close(void* h) { FreeLibrary((HMODULE)h); }
static const char* lib_error(void) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "error %lu", (unsigned long)GetLastError());
    return buf;
}
#else
#include <dlfcn.h>
static void* lib_open(const char* path) { return dlopen(path, RTLD_NOW); }
static void* lib_sym(void* h, const char* name) { return dlsym(h, name); }
static void lib_close(void* h) { dlclose(h); }
static const char* lib_error(void) { return dlerror(); }
#endif

#include "aether_config.h"

typedef AetherValue* (*build_tree_fn)(void);

#define FAIL(fmt, ...) do { \
    fprintf(stderr, "FAIL (line %d): " fmt "\n", __LINE__, ##__VA_ARGS__); \
    return 1; \
} while (0)

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-lib>\n", argv[0]);
        return 2;
    }

    void* h = lib_open(argv[1]);
    if (!h) FAIL("loading %s: %s", argv[1], lib_error());

    build_tree_fn build = (build_tree_fn)lib_sym(h, "aether_build_tree");
    if (!build) FAIL("aether_build_tree not found: %s", lib_error());

    /* (1) NULL probe — must not crash, must return UNKNOWN. */
    if (aether_value_kind(NULL) != AETHER_KIND_UNKNOWN)
        FAIL("kind(NULL) != UNKNOWN");
    if (aether_value_is_map(NULL) != 0)  FAIL("is_map(NULL) != 0");
    if (aether_value_is_list(NULL) != 0) FAIL("is_list(NULL) != 0");

    /* (2) Adversarial probe — small intptr-cast scalar. The low-
     * address guard MUST filter this before any deref. If the
     * guard is wrong, the process segfaults reading address 0x2A. */
    AetherValue* fake_int_value = (AetherValue*)(uintptr_t)42;
    if (aether_value_kind(fake_int_value) != AETHER_KIND_UNKNOWN)
        FAIL("kind(low-addr scalar) != UNKNOWN");
    /* Try a few more low-address values for thoroughness. */
    if (aether_value_kind((AetherValue*)(uintptr_t)0x1)    != AETHER_KIND_UNKNOWN) FAIL("kind(0x1)");
    if (aether_value_kind((AetherValue*)(uintptr_t)0xFF)   != AETHER_KIND_UNKNOWN) FAIL("kind(0xFF)");
    if (aether_value_kind((AetherValue*)(uintptr_t)0xFFFF) != AETHER_KIND_UNKNOWN) FAIL("kind(0xFFFF)");

    AetherValue* root = build();
    if (!root) FAIL("aether_build_tree returned NULL");

    /* (3-5) Root is a map. */
    if (aether_value_kind(root) != AETHER_KIND_MAP) FAIL("kind(root) != MAP");
    if (aether_value_is_map(root)  != 1) FAIL("is_map(root) != 1");
    if (aether_value_is_list(root) != 0) FAIL("is_list(root) != 0");

    /* (6) Nested map. */
    AetherValue* addr = aether_config_get_map(root, "addr");
    if (!addr) FAIL("addr was NULL");
    if (aether_value_kind(addr) != AETHER_KIND_MAP) FAIL("kind(addr) != MAP");

    /* (7) Nested list. */
    AetherValue* tags = aether_config_get_list(root, "tags");
    if (!tags) FAIL("tags was NULL");
    if (aether_value_kind(tags) != AETHER_KIND_LIST) FAIL("kind(tags) != LIST");

    /* (8) The int slot ("age") was stored as (void*)(intptr_t)42.
     * If we (mistakenly) call _get_map on it, we get back a non-
     * container pointer. The kind probe must safely return UNKNOWN
     * — no segfault, no false MAP/LIST claim. This is the whole
     * point of the safety contract: schema-mismatch survives. */
    AetherValue* age_misread = aether_config_get_map(root, "age");
    /* age_misread holds (intptr_t)42 cast back to AetherValue*. */
    if (aether_value_kind(age_misread) != AETHER_KIND_UNKNOWN)
        FAIL("kind(age-as-map-misread) != UNKNOWN");

    /* (9) Deep-free. After this, the map's magic is cleared, and
     * a re-probe MUST report UNKNOWN — the use-after-free guard. */
    aether_config_free_deep(root);
    /* NB: probing freed memory is technically UB at the C level
     * because the allocation itself has been released. In practice
     * the magic-clear we do BEFORE the free call means a stale
     * read still sees the cleared magic before the heap reclaim.
     * In ASAN this would still flag — that's correct, it's still
     * a stale read. The host is expected to drop the pointer
     * after a deep-free; this assertion documents the cleared
     * magic, not a supported usage pattern. */
    /* Skip the post-free probe under sanitizers — the heap-reclaim
     * makes any read invalid even though the magic was cleared. */
#ifndef __SANITIZE_ADDRESS__
    if (aether_value_kind(root) != AETHER_KIND_UNKNOWN)
        FAIL("kind(root) post-free != UNKNOWN (magic-clear failed)");
#endif

    lib_close(h);
    printf("OK: kind predicates safe; deep-free walks containers; magic cleared on free\n");
    return 0;
}
