/*
 * consume.c — C host for the typed-struct-pointer --emit=lib test.
 *
 * The exported wrapper's ABI presents the `*Thing` parameter as an opaque
 * handle (AetherValue*), so a C host passes a pointer to a layout-compatible
 * struct. The wrapper casts it back to Thing* internally. We verify the call
 * both links and behaves: bump() returns the old n and increments in place.
 */

#include <stdio.h>
#include <stdint.h>

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

/* Layout-compatible with Aether's `Thing` struct (one int field). The exported
 * ABI parameter is an opaque handle, so the host passes the address of a
 * matching struct and the wrapper casts it back to Thing internally. */
struct HostThing { int32_t n; };

typedef int32_t (*aether_bump_fn)(void*);

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-lib>\n", argv[0]);
        return 2;
    }
    void* h = lib_open(argv[1]);
    if (!h) { fprintf(stderr, "FAIL: loading the library: %s\n", lib_error()); return 1; }

    aether_bump_fn bump = (aether_bump_fn)lib_sym(h, "aether_bump");
    if (!bump) { fprintf(stderr, "FAIL: aether_bump not found: %s\n", lib_error()); return 1; }

    struct HostThing t; t.n = 41;
    int32_t r1 = bump(&t);          /* returns old n (41), t.n -> 42 */
    int32_t r2 = bump(&t);          /* returns 42,        t.n -> 43 */

    if (r1 != 41) { fprintf(stderr, "FAIL: bump#1 returned %d, want 41\n", r1); return 1; }
    if (r2 != 42) { fprintf(stderr, "FAIL: bump#2 returned %d, want 42\n", r2); return 1; }
    if (t.n != 43) { fprintf(stderr, "FAIL: t.n is %d, want 43 (in-place mutation)\n", t.n); return 1; }

    printf("OK\n");
    lib_close(h);
    return 0;
}
