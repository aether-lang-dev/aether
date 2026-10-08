/* Force-included (-include) into the generated C of the #2480 probes.
 *
 * Counts closure environments and promoted capture cells by hooking the
 * allocator calls the generated code makes: an env is the
 * `malloc(sizeof(_closure_env_N))` of a closure literal and a cell the
 * `calloc(1, sizeof(_AeCellHeader) + size)` of a promoted capture, both
 * recognised by the spelling of their size argument. Every free() in the
 * generated code (the env destructors and the cell releases are generated
 * code) is checked against those blocks.
 *
 * A freed env or cell is not returned to the allocator: it is filled with a
 * poison byte and kept, so a later read through a dangling env sees the
 * poison deterministically (a captured int prints as garbage, a captured
 * pointer faults) instead of whatever reused the memory. A second free of the
 * same block is counted as a double free.
 *
 * At exit it prints one line to stderr:
 *   ENVCOUNT envs=<allocated> env_frees=<freed> env_live=<never freed>
 *            cells=<n> cell_frees=<n> cell_live=<n> double_frees=<n>
 */
#ifndef AE_ENV_COUNT_H
#define AE_ENV_COUNT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#endif

#define EC_MAX 65536

typedef struct {
    void* p;
    size_t size;
    int kind;      /* 1 env, 2 cell */
    int freed;
} EcBlock;

static EcBlock ec_blocks[EC_MAX];
static int ec_count = 0;
static int ec_double = 0;
static int ec_overflow = 0;
static int ec_registered = 0;
/* Actor handlers allocate and free on scheduler threads. */
static volatile char ec_lock_flag = 0;
static void ec_lock(void) { while (__atomic_test_and_set(&ec_lock_flag, __ATOMIC_ACQUIRE)) { } }
static void ec_unlock(void) { __atomic_clear(&ec_lock_flag, __ATOMIC_RELEASE); }

static void ec_report(void) {
    int envs = 0, env_frees = 0, cells = 0, cell_frees = 0;
    for (int i = 0; i < ec_count; i++) {
        if (ec_blocks[i].kind == 1) { envs++; env_frees += ec_blocks[i].freed; }
        else { cells++; cell_frees += ec_blocks[i].freed; }
    }
    fprintf(stderr,
            "ENVCOUNT envs=%d env_frees=%d env_live=%d cells=%d cell_frees=%d cell_live=%d double_frees=%d%s\n",
            envs, env_frees, envs - env_frees, cells, cell_frees, cells - cell_frees,
            ec_double, ec_overflow ? " overflow" : "");
}

/* Registered before main, so a program that builds no env still reports. */
__attribute__((constructor)) static void ec_register(void) {
    if (!ec_registered) { ec_registered = 1; atexit(ec_report); }
}

static void* ec_track(void* p, size_t size, int kind) {
    if (!p) return p;
    ec_lock();
    if (ec_count >= EC_MAX) {
        ec_overflow = 1;
    } else {
        ec_blocks[ec_count].p = p;
        ec_blocks[ec_count].size = size;
        ec_blocks[ec_count].kind = kind;
        ec_blocks[ec_count].freed = 0;
        ec_count++;
    }
    ec_unlock();
    return p;
}

static void* ec_malloc(size_t n, const char* spelled) {
    void* p = malloc(n);
    if (strstr(spelled, "_closure_env_")) return ec_track(p, n, 1);
    return p;
}

static void* ec_calloc(size_t k, size_t n, const char* spelled) {
    void* p = calloc(k, n);
    if (strstr(spelled, "_AeCellHeader")) return ec_track(p, k * n, 2);
    return p;
}

static void ec_free(void* p) {
    if (!p) return;
    ec_lock();
    /* Newest first: a block is most often freed soon after it is made. */
    for (int i = ec_count - 1; i >= 0; i--) {
        if (ec_blocks[i].p != p) continue;
        if (ec_blocks[i].freed) {
            ec_double++;
        } else {
            ec_blocks[i].freed = 1;
            memset(p, 0xA5, ec_blocks[i].size);
        }
        ec_unlock();
        return;
    }
    ec_unlock();
    free(p);
}

#define malloc(n) ec_malloc((n), #n)
#define calloc(k, n) ec_calloc((k), (n), #n)
#define free(p) ec_free((void*)(p))

#endif
