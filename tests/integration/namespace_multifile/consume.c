/*
 * consume.c — multi-script namespace.
 *
 * Three sibling scripts (add, multiply, subtract) all contribute to
 * the calc namespace. The C consumer dlopens the lib and verifies
 * every aether_<name>() symbol is callable, plus the shared event
 * fires from each one with its own id.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

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

typedef int32_t (*binop_fn)(int32_t, int32_t);

#define FAIL(fmt, ...) do { \
    fprintf(stderr, "FAIL (line %d): " fmt "\n", __LINE__, ##__VA_ARGS__); \
    return 1; \
} while (0)

static int64_t g_last_id = 0;
static int     g_calls   = 0;
static void on_computed(int64_t id) { g_last_id = id; g_calls++; }

int main(int argc, char** argv) {
    if (argc < 2) return 2;

    void* h = lib_open(argv[1]);
    if (!h) FAIL("loading %s: %s", argv[1], lib_error());

    binop_fn add = (binop_fn)lib_sym(h, "aether_add");
    binop_fn mul = (binop_fn)lib_sym(h, "aether_multiply");
    binop_fn sub = (binop_fn)lib_sym(h, "aether_subtract");
    if (!add || !mul || !sub) FAIL("symbol lookup (%s)", lib_error());

    aether_event_register("Computed", on_computed);

    if (add(3, 4) != 7)        FAIL("add(3,4) != 7");
    if (g_last_id != 1)        FAIL("Computed id after add = %lld, expected 1", (long long)g_last_id);

    if (mul(6, 7) != 42)       FAIL("mul(6,7) != 42");
    if (g_last_id != 2)        FAIL("Computed id after mul = %lld, expected 2", (long long)g_last_id);

    if (sub(10, 3) != 7)       FAIL("sub(10,3) != 7");
    if (g_last_id != 3)        FAIL("Computed id after sub = %lld, expected 3", (long long)g_last_id);

    if (g_calls != 3)          FAIL("expected 3 handler calls, got %d", g_calls);

    /* Discovery still works — same Computed event surfaces in the manifest. */
    typedef const AetherNamespaceManifest* (*describe_fn)(void);
    describe_fn describe = (describe_fn)lib_sym(h, "aether_describe");
    if (!describe) FAIL("aether_describe missing");
    const AetherNamespaceManifest* m = describe();
    if (strcmp(m->namespace_name, "calc") != 0)
        FAIL("namespace = %s, expected \"calc\"", m->namespace_name);
    if (m->event_count != 1) FAIL("event_count = %d", m->event_count);

    lib_close(h);
    printf("OK: namespace_multifile — three scripts share one namespace\n");
    return 0;
}
