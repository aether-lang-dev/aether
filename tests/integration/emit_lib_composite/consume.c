/*
 * consume.c — Composite-return round-trip test.
 *
 * Calls aether_build_config("prod", 8080), then walks the returned tree
 * via the aether_config_* accessors and asserts every field.
 *
 * This is the flagship test for the --emit=lib design: if the host can't
 * cleanly walk structured data returned from an Aether script, the whole
 * config/rules-engine use case is a non-starter.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "aether_config.h"

/* The library is loaded by path on every platform: dlopen on POSIX,
 * LoadLibrary on Windows, where the test used to be skipped and so never
 * saw the accessors read a string the script had copied. */
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

typedef AetherValue* (*build_config_fn)(const char*, int32_t);

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

    build_config_fn build = (build_config_fn)lib_sym(h, "aether_build_config");
    if (!build) FAIL("aether_build_config not found: %s", lib_error());

    AetherValue* root = build("prod", 8080);
    if (!root) FAIL("aether_build_config returned NULL");

    /* Top-level string */
    const char* env = aether_config_get_string(root, "env");
    if (!env || strcmp(env, "prod") != 0) FAIL("env = %s, expected \"prod\"", env ? env : "(null)");

    /* Top-level int */
    int32_t port = aether_config_get_int(root, "port", -1);
    if (port != 8080) FAIL("port = %d, expected 8080", port);

    /* Missing key returns the supplied default */
    int32_t missing = aether_config_get_int(root, "does_not_exist", 999);
    if (missing != 999) FAIL("missing-key default = %d, expected 999", missing);

    /* has() */
    if (!aether_config_has(root, "env"))  FAIL("has(env) returned 0");
    if (aether_config_has(root, "xxxxx")) FAIL("has(nonexistent) returned 1");

    /* Nested map */
    AetherValue* db = aether_config_get_map(root, "db");
    if (!db) FAIL("db sub-map returned NULL");
    const char* db_host = aether_config_get_string(db, "host");
    if (!db_host || strcmp(db_host, "db.internal") != 0)
        FAIL("db.host = %s, expected \"db.internal\"", db_host ? db_host : "(null)");
    int32_t db_port = aether_config_get_int(db, "port", -1);
    if (db_port != 5432) FAIL("db.port = %d, expected 5432", db_port);

    /* Nested list */
    AetherValue* flags = aether_config_get_list(root, "flags");
    if (!flags) FAIL("flags list returned NULL");
    int32_t n = aether_config_list_size(flags);
    if (n != 3) FAIL("flags list size = %d, expected 3", n);

    const char* f0 = aether_config_list_get_string(flags, 0);
    const char* f1 = aether_config_list_get_string(flags, 1);
    const char* f2 = aether_config_list_get_string(flags, 2);
    if (!f0 || strcmp(f0, "dark_mode")    != 0) FAIL("flags[0] = %s", f0 ? f0 : "(null)");
    if (!f1 || strcmp(f1, "new_billing")  != 0) FAIL("flags[1] = %s", f1 ? f1 : "(null)");
    if (!f2 || strcmp(f2, "v2_ui")        != 0) FAIL("flags[2] = %s", f2 ? f2 : "(null)");

    /* Out-of-range list index returns NULL/default */
    if (aether_config_list_get_string(flags, 42) != NULL) FAIL("out-of-range list_get_string returned non-NULL");
    if (aether_config_list_get_int(flags, 42, 7) != 7)    FAIL("out-of-range list_get_int didn't return default");

    /* Defensive NULL handling */
    if (aether_config_get_string(NULL, "x") != NULL) FAIL("get_string(NULL) didn't return NULL");
    if (aether_config_get_int(NULL, "x", 5) != 5)    FAIL("get_int(NULL) didn't return default");
    if (aether_config_has(NULL, "x") != 0)           FAIL("has(NULL) didn't return 0");
    if (aether_config_list_size(NULL) != 0)          FAIL("list_size(NULL) didn't return 0");

    aether_config_free(root);
    lib_close(h);
    printf("OK: composite config tree round-tripped\n");
    return 0;
}
