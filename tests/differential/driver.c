/* Differential-test driver (#523): runs a `--emit=lib` artifact's entry point.
 *
 * `--emit=lib` deliberately omits main(), so the lib half of a differential
 * comparison cannot just be executed. Every top-level Aether function is
 * exported as `aether_<name>`, so each case exposes `run()` and this driver
 * calls `aether_run` through it. The exe half runs the same `run()` via the
 * case's own main().
 *
 * One driver serves every case: the entry point is resolved by name at
 * runtime, so no per-case generation is needed. */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: driver <library>\n");
        return 2;
    }

    void* handle = dlopen(argv[1], RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "driver: dlopen failed: %s\n", dlerror());
        return 2;
    }

    /* The cast goes through a union-free two-step because ISO C has no
       conversion between object and function pointers; every POSIX platform
       this driver builds on defines dlsym's return to be callable. */
    void* sym = dlsym(handle, "aether_run");
    if (!sym) {
        fprintf(stderr, "driver: no aether_run export: %s\n", dlerror());
        dlclose(handle);
        return 2;
    }
    void (*entry)(void);
    *(void**)(&entry) = sym;

    entry();
    fflush(stdout);
    return 0;
}
