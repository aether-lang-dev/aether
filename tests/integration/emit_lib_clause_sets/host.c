/*
 * host.c: loads the library built from clauses.ae (dlopen, or LoadLibrary on
 * Windows), checks that every symbol named on the command line resolves,
 * and calls the exports. Prints OK, or FAIL with context.
 */

#include <stdio.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
static void* lib_open(const char* path) { return (void*)LoadLibraryA(path); }
static void* lib_sym(void* h, const char* name) {
    return (void*)GetProcAddress((HMODULE)h, name);
}
#else
#include <dlfcn.h>
static void* lib_open(const char* path) { return dlopen(path, RTLD_NOW); }
static void* lib_sym(void* h, const char* name) { return dlsym(h, name); }
#endif

typedef int32_t (*Fn1)(int32_t);
typedef int32_t (*Fn2)(int32_t, int32_t);

static int failures = 0;

static void expect(const char* what, int32_t got, int32_t want) {
    if (got != want) {
        printf("FAIL: %s = %d, want %d\n", what, (int)got, (int)want);
        failures++;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("FAIL: usage: host <library> [symbol...]\n"); return 1; }
    void* h = lib_open(argv[1]);
    if (!h) { printf("FAIL: cannot load %s\n", argv[1]); return 1; }
    /* Every symbol the catalog names is one the library defines. */
    for (int i = 2; i < argc; i++) {
        if (!lib_sym(h, argv[i])) {
            printf("FAIL: the catalog names %s, which the library does not export\n", argv[i]);
            failures++;
        }
    }
    Fn1 sign = (Fn1)lib_sym(h, "aether_sign");
    Fn1 fact = (Fn1)lib_sym(h, "aether_fact");
    Fn1 triple_alias = (Fn1)lib_sym(h, "aether_triple");
    Fn1 triple = (Fn1)lib_sym(h, "cl_triple");
    Fn1 pick = (Fn1)lib_sym(h, "cl_pick");
    Fn2 add = (Fn2)lib_sym(h, "aether_add");
    Fn1 plain = (Fn1)lib_sym(h, "aether_plain");
    if (!sign || !fact || !triple_alias || !triple || !pick || !add || !plain) {
        printf("FAIL: missing export: sign=%p fact=%p aether_triple=%p cl_triple=%p cl_pick=%p add=%p plain=%p\n",
               (void*)sign, (void*)fact, (void*)triple_alias, (void*)triple, (void*)pick,
               (void*)add, (void*)plain);
        return 1;
    }
    expect("aether_sign(-5)", sign(-5), -1);
    expect("aether_sign(3)", sign(3), 1);
    expect("aether_fact(5)", fact(5), 120);
    expect("aether_fact(0)", fact(0), 1);
    expect("aether_triple(4)", triple_alias(4), 12);
    expect("cl_triple(4)", triple(4), 12);
    expect("cl_pick(0)", pick(0), 100);
    expect("cl_pick(4)", pick(4), 5);
    expect("aether_add(2, 3)", add(2, 3), 5);
    expect("aether_plain(1)", plain(1), 2);
    if (failures) return 1;
    printf("OK\n");
    return 0;
}
