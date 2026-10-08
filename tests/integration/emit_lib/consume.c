/*
 * consume.c — Minimal C host for the --emit=lib round-trip test.
 *
 * Loads the shared library built from config.ae (dlopen, or LoadLibrary
 * on Windows), resolves the aether_* symbols, and calls them. Prints OK on
 * success, FAIL with context on any mismatch.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

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

typedef int32_t (*aether_sum_fn)(int32_t, int32_t);
typedef const char* (*aether_greet_fn)(const char*);
typedef int32_t (*aether_multiply_by_two_fn)(int32_t);

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-libconfig.so>\n", argv[0]);
        return 2;
    }

    void* h = lib_open(argv[1]);
    if (!h) {
        fprintf(stderr, "FAIL: loading %s: %s\n", argv[1], lib_error());
        return 1;
    }

    aether_sum_fn sum = (aether_sum_fn)lib_sym(h, "aether_sum");
    aether_greet_fn greet = (aether_greet_fn)lib_sym(h, "aether_greet");
    aether_multiply_by_two_fn mul2 = (aether_multiply_by_two_fn)lib_sym(h, "aether_multiply_by_two");

    if (!sum)   { fprintf(stderr, "FAIL: aether_sum not found: %s\n", lib_error()); return 1; }
    if (!greet) { fprintf(stderr, "FAIL: aether_greet not found: %s\n", lib_error()); return 1; }
    if (!mul2)  { fprintf(stderr, "FAIL: aether_multiply_by_two not found: %s\n", lib_error()); return 1; }

    int32_t s = sum(7, 35);
    if (s != 42) {
        fprintf(stderr, "FAIL: aether_sum(7, 35) = %d, expected 42\n", s);
        return 1;
    }

    const char* g = greet("hello");
    if (!g || strcmp(g, "hello") != 0) {
        fprintf(stderr, "FAIL: aether_greet(\"hello\") = %s, expected \"hello\"\n", g ? g : "(null)");
        return 1;
    }

    int32_t m = mul2(21);
    if (m != 42) {
        fprintf(stderr, "FAIL: aether_multiply_by_two(21) = %d, expected 42\n", m);
        return 1;
    }

    lib_close(h);
    printf("OK: all three aether_* entry points round-tripped\n");
    return 0;
}
