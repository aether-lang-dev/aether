/*
 * consume.c — Verifies the notify() claim-check primitive.
 *
 * Registers handlers for two events, leaves one event unhandled,
 * dlopens the Aether-built lib, calls each emit_*() function, and
 * asserts:
 *   - Registered handlers fire with the correct id.
 *   - notify() returns 1 for handled events, 0 for unhandled.
 *   - Re-registering replaces the prior handler.
 *   - aether_event_unregister() removes a handler.
 *   - aether_event_clear() drops all handlers.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

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

#include "aether_host.h"

typedef int32_t (*emit_fn)(int64_t);

#define FAIL(fmt, ...) do { \
    fprintf(stderr, "FAIL (line %d): " fmt "\n", __LINE__, ##__VA_ARGS__); \
    return 1; \
} while (0)

/* Side-channel state captured by handlers so the test can verify them. */
static int64_t g_one_seen = -1;
static int     g_one_calls = 0;
static int64_t g_two_seen = -1;
static int64_t g_one_seen_replacement = -1;

static void handler_one(int64_t id)         { g_one_seen = id; g_one_calls++; }
static void handler_two(int64_t id)         { g_two_seen = id; }
static void handler_one_replacement(int64_t id) { g_one_seen_replacement = id; }

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <lib>\n", argv[0]); return 2; }

    void* h = lib_open(argv[1]);
    if (!h) FAIL("loading %s: %s", argv[1], lib_error());

    emit_fn emit_one     = (emit_fn)lib_sym(h, "aether_emit_one");
    emit_fn emit_two     = (emit_fn)lib_sym(h, "aether_emit_two");
    emit_fn emit_unknown = (emit_fn)lib_sym(h, "aether_emit_unknown");
    if (!emit_one || !emit_two || !emit_unknown) FAIL("symbol lookup: %s", lib_error());

    /* Register two of three. Leave NoListener unhandled. */
    if (aether_event_register("OneEvent", handler_one) != 0) FAIL("register OneEvent");
    if (aether_event_register("TwoEvent", handler_two) != 0) FAIL("register TwoEvent");

    /* Round-trip 1: registered handlers fire and notify() returns 1. */
    int32_t r1 = emit_one(111);
    if (r1 != 1)         FAIL("emit_one returned %d, expected 1", r1);
    if (g_one_seen != 111) FAIL("OneEvent handler saw %lld, expected 111", (long long)g_one_seen);

    int32_t r2 = emit_two(222);
    if (r2 != 1)         FAIL("emit_two returned %d, expected 1", r2);
    if (g_two_seen != 222) FAIL("TwoEvent handler saw %lld, expected 222", (long long)g_two_seen);

    /* Round-trip 2: unhandled event returns 0, no crash. */
    int32_t r3 = emit_unknown(333);
    if (r3 != 0) FAIL("emit_unknown returned %d, expected 0", r3);

    /* Re-registration replaces the prior handler in place. */
    if (aether_event_register("OneEvent", handler_one_replacement) != 0)
        FAIL("re-register OneEvent");
    g_one_calls = 0;
    int32_t r4 = emit_one(444);
    if (r4 != 1) FAIL("emit_one after re-register returned %d", r4);
    if (g_one_seen_replacement != 444)
        FAIL("replacement handler saw %lld, expected 444", (long long)g_one_seen_replacement);
    if (g_one_calls != 0)
        FAIL("original handler still fired after re-register (calls=%d)", g_one_calls);

    /* Unregister leaves no handler. */
    if (aether_event_unregister("OneEvent") != 0) FAIL("unregister OneEvent");
    int32_t r5 = emit_one(555);
    if (r5 != 0) FAIL("emit_one after unregister returned %d, expected 0", r5);

    /* Unregistering an unknown event returns -1, no crash. */
    if (aether_event_unregister("NeverRegistered") != -1)
        FAIL("unregister of unknown event should return -1");

    /* clear() drops everything. */
    aether_event_clear();
    int32_t r6 = emit_two(666);
    if (r6 != 0) FAIL("emit_two after clear returned %d, expected 0", r6);

    /* Defensive: NULL event name returns 0. */
    if (notify(NULL, 0) != 0) FAIL("notify(NULL, ...) didn't return 0");

    /* Defensive: NULL handler / NULL name on register returns -1. */
    if (aether_event_register(NULL, handler_one) != -1) FAIL("register NULL name");
    if (aether_event_register("X", NULL) != -1)         FAIL("register NULL handler");

    lib_close(h);
    printf("OK: notify() claim-check round-trip\n");
    return 0;
}
