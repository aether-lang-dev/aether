/* @c_callback functions by name (#2297).
 *
 * Every program and --emit=lib library registers its @c_callback functions
 * when it loads (a constructor codegen emits), and the runtime resolves a
 * hook a program supplies, such as the pure-TLS client and server, through
 * aether_callback_lookup before falling back to its own definition.
 *
 * A weak definition overridden at link time only works when the runtime is
 * linked INTO the program. A shared runtime (aether.dll, libaether.dylib)
 * binds its own calls to its own symbols, so there the override has to arrive
 * by registration. This file stands alone because the compiler links the
 * standard library's objects without the rest of the runtime, and the
 * standard library's hook call sites need it.
 *
 * Written by constructors (one image loading at a time, under the loader's
 * lock) and read from any thread: an entry is filled in before the count that
 * publishes it is released, and a re-registration only swaps the pointer,
 * atomically. Fixed capacity: hooks are a handful of names, and a full table
 * drops the newest registration (the runtime's own definition then stays in
 * use) rather than allocate in a constructor. */

#include "aether_callbacks.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#define AETHER_CALLBACK_MAX 256

typedef struct {
    const char* name;      /* a string literal in the registering image */
    _Atomic(void*) fn;
} AetherCallbackEntry;

static AetherCallbackEntry g_callbacks[AETHER_CALLBACK_MAX];
static atomic_int g_callback_count = 0;

void aether_callback_register(const char* name, void* fn) {
    if (!name || !fn) return;
    int n = atomic_load_explicit(&g_callback_count, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        if (strcmp(g_callbacks[i].name, name) == 0) {
            atomic_store_explicit(&g_callbacks[i].fn, fn, memory_order_release);
            return;
        }
    }
    if (n >= AETHER_CALLBACK_MAX) return;
    g_callbacks[n].name = name;
    atomic_store_explicit(&g_callbacks[n].fn, fn, memory_order_relaxed);
    atomic_store_explicit(&g_callback_count, n + 1, memory_order_release);
}

void* aether_callback_lookup(const char* name) {
    if (!name) return NULL;
    int n = atomic_load_explicit(&g_callback_count, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        if (strcmp(g_callbacks[i].name, name) == 0)
            return atomic_load_explicit(&g_callbacks[i].fn, memory_order_acquire);
    }
    return NULL;
}
