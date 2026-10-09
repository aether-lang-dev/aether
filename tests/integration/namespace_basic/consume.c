/*
 * consume.c — End-to-end namespace round-trip test.
 *
 * Verifies that `ae build --namespace` produces a .so where:
 *   - aether_describe() reports the correct manifest
 *   - exported aether_<name>() functions are callable
 *   - registered event handlers receive notify() calls
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "aether_host.h"

/* The library is loaded by path on every platform: dlopen on POSIX,
 * LoadLibrary on Windows. It is also linked, for aether_event_register, so
 * the load returns the module already mapped and both see one registry. */
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

typedef const AetherNamespaceManifest* (*describe_fn)(void);
typedef const char* (*say_hi_fn)(const char*);

#define FAIL(fmt, ...) do { \
    fprintf(stderr, "FAIL (line %d): " fmt "\n", __LINE__, ##__VA_ARGS__); \
    return 1; \
} while (0)

static int64_t g_last_id = -1;
static void on_greeted(int64_t id) { g_last_id = id; }

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <lib>\n", argv[0]); return 2; }

    void* h = lib_open(argv[1]);
    if (!h) FAIL("loading %s: %s", argv[1], lib_error());

    describe_fn describe = (describe_fn)lib_sym(h, "aether_describe");
    say_hi_fn say_hi    = (say_hi_fn)lib_sym(h, "aether_say_hi");
    if (!describe) FAIL("aether_describe missing: %s", lib_error());
    if (!say_hi)   FAIL("aether_say_hi missing: %s", lib_error());

    /* aether_describe matches what manifest.ae declared. */
    const AetherNamespaceManifest* m = describe();
    if (!m) FAIL("aether_describe returned NULL");
    if (!m->namespace_name || strcmp(m->namespace_name, "greet") != 0)
        FAIL("namespace = %s, expected \"greet\"", m->namespace_name ? m->namespace_name : "(null)");
    if (m->input_count != 1) FAIL("input_count = %d, expected 1", m->input_count);
    if (strcmp(m->inputs[0].name, "greeting") != 0)
        FAIL("input[0].name = %s", m->inputs[0].name);
    if (strcmp(m->inputs[0].type_signature, "string") != 0)
        FAIL("input[0].type = %s", m->inputs[0].type_signature);
    if (m->event_count != 1) FAIL("event_count = %d, expected 1", m->event_count);
    if (strcmp(m->events[0].name, "Greeted") != 0)
        FAIL("event[0].name = %s", m->events[0].name);
    if (strcmp(m->java.package_name, "com.example.greet") != 0)
        FAIL("java.package = %s", m->java.package_name);
    if (strcmp(m->java.class_name, "Greeter") != 0)
        FAIL("java.class = %s", m->java.class_name);

    /* Round-trip: registered handler fires, exported function returns. */
    aether_event_register("Greeted", on_greeted);
    const char* r = say_hi("alice");
    if (!r || strcmp(r, "alice") != 0) FAIL("say_hi returned %s", r ? r : "(null)");
    if (g_last_id != 42) FAIL("Greeted handler last_id = %lld, expected 42", (long long)g_last_id);

    lib_close(h);
    printf("OK: namespace_basic — describe, downcall, notify\n");
    return 0;
}
