/*
 * consume.c — Verifies the manifest builder DSL.
 *
 * loads the lib, calls aether_abi() (which runs the namespace()
 * block), then walks the captured manifest via manifest_get() and
 * asserts each input/event/binding is present.
 */

#include <stdio.h>
#include <string.h>

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

#include "aether_host.h"

typedef void (*setup_fn)(void);

#define FAIL(fmt, ...) do { \
    fprintf(stderr, "FAIL (line %d): " fmt "\n", __LINE__, ##__VA_ARGS__); \
    return 1; \
} while (0)

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <lib>\n", argv[0]); return 2; }

    /* Belt-and-braces: clear any state the host shares with the lib. */
    manifest_clear();

    void* h = lib_open(argv[1]);
    if (!h) FAIL("loading the library: %s", lib_error());

    setup_fn abi = (setup_fn)lib_sym(h, "aether_abi");
    if (!abi) FAIL("aether_abi not found: %s", lib_error());

    /* Run the manifest builder. */
    abi();

    AetherManifest* m = manifest_get();
    if (!m) FAIL("manifest_get returned NULL — namespace() didn't run");

    /* Namespace name */
    if (!m->namespace_name || strcmp(m->namespace_name, "trading") != 0)
        FAIL("namespace_name = %s, expected \"trading\"", m->namespace_name ? m->namespace_name : "(null)");

    /* Inputs */
    if (m->input_count != 3) FAIL("input_count = %d, expected 3", m->input_count);
    /* Order matters — they should appear in declaration order. */
    if (strcmp(m->inputs[0].name, "order") != 0
     || strcmp(m->inputs[0].type_signature, "map") != 0)
        FAIL("inputs[0] = (%s, %s)", m->inputs[0].name, m->inputs[0].type_signature);
    if (strcmp(m->inputs[1].name, "catalog_has") != 0
     || strcmp(m->inputs[1].type_signature, "fn(string) -> bool") != 0)
        FAIL("inputs[1] = (%s, %s)", m->inputs[1].name, m->inputs[1].type_signature);
    if (strcmp(m->inputs[2].name, "max_order") != 0
     || strcmp(m->inputs[2].type_signature, "int") != 0)
        FAIL("inputs[2] = (%s, %s)", m->inputs[2].name, m->inputs[2].type_signature);

    /* Events */
    if (m->event_count != 3) FAIL("event_count = %d, expected 3", m->event_count);
    if (strcmp(m->events[0].name, "OrderPlaced") != 0)   FAIL("events[0] = %s", m->events[0].name);
    if (strcmp(m->events[1].name, "OrderRejected") != 0) FAIL("events[1] = %s", m->events[1].name);
    if (strcmp(m->events[2].name, "UnknownTicker") != 0) FAIL("events[2] = %s", m->events[2].name);
    for (int i = 0; i < 3; i++) {
        if (strcmp(m->events[i].carries_type, "int64") != 0)
            FAIL("events[%d] carries %s, expected \"int64\"", i, m->events[i].carries_type);
    }

    /* Bindings */
    if (!m->java.package_name || strcmp(m->java.package_name, "com.example.trading") != 0)
        FAIL("java.package = %s", m->java.package_name ? m->java.package_name : "(null)");
    if (!m->java.class_name || strcmp(m->java.class_name, "Trading") != 0)
        FAIL("java.class = %s", m->java.class_name ? m->java.class_name : "(null)");
    if (!m->python.module_name || strcmp(m->python.module_name, "trading_rules") != 0)
        FAIL("python.module = %s", m->python.module_name ? m->python.module_name : "(null)");
    if (!m->go.package_name || strcmp(m->go.package_name, "trading") != 0)
        FAIL("go.package = %s", m->go.package_name ? m->go.package_name : "(null)");

    /* clear() resets and manifest_get() returns NULL again. */
    manifest_clear();
    if (manifest_get() != NULL)
        FAIL("manifest_get() should return NULL after clear");

    lib_close(h);
    printf("OK: manifest builders captured all fields\n");
    return 0;
}
