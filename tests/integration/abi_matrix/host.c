/* A C host of mlib.ae across versions (#2957, docs/abi-policy.md). It is
 * compiled against the libaether.h of this Aether (host ABI level 1 or
 * more) or of the release before it, loads the library given (built by
 * either) and reads everything by name, as a plugin host does. argv[2],
 * when given, is the shared runtime a --shared-runtime library runs on,
 * where the runtime's functions are; otherwise they are the library's own.
 * argv[3] = --runtime-abi prints only the library's recorded runtime ABI ID.
 * It uses a level-1 function only when the header it was compiled against
 * has it and the library's catalog records level 1 or more. What it prints
 * depends on the levels alone, never on the versions:
 *
 *   catalog:   the schema, and the host ABI level the library records
 *              (unknown to a host built with level-0 headers)
 *   preflight: the library's <lib>_check, where it has one (level 1)
 *   typed:     int, float, long and bool arguments and results
 *   strings:   a string the host owns, given back through the library's
 *              release, and 20000 more: the library's runtime holds no
 *              byte more than before (its count of the memory it charges)
 *   errors:    a panic ends the call, not the process (level 1)
 *   deadline:  a deadline ends a loop (level 1; a level-0 host asks only
 *              whether the loop was cut)
 *   unload:    the library unloads after all this
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "libaether.h"
#include "aether_lib_meta.h"
#ifdef _WIN32
#include <windows.h>
static void* lib_open(const char* p) { return (void*)LoadLibraryA(p); }
static void* lib_open_global(const char* p) { return lib_open(p); }
static void* lib_sym(void* h, const char* n) { return (void*)GetProcAddress((HMODULE)h, n); }
static int lib_close(void* h) { return FreeLibrary((HMODULE)h) ? 0 : -1; }
static void lib_why(void) { fprintf(stderr, "LoadLibrary error %lu\n", (unsigned long)GetLastError()); }
#else
#include <dlfcn.h>
static void* lib_open(const char* p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void* lib_open_global(const char* p) { return dlopen(p, RTLD_NOW | RTLD_GLOBAL); }
static void* lib_sym(void* h, const char* n) { return dlsym(h, n); }
static int lib_close(void* h) { return dlclose(h); }
static void lib_why(void) { const char* e = dlerror(); fprintf(stderr, "%s\n", e ? e : "(no dlerror)"); }
#endif

#ifdef AETHER_HOST_ABI
#define HEADER_LEVEL AETHER_HOST_ABI
#else
#define HEADER_LEVEL 0
#endif

typedef int (*add_fn)(int, int);
typedef double (*scale_fn)(double, double);
typedef int64_t (*wide_fn)(int64_t);
typedef int (*even_fn)(int);
typedef const void* (*greet_fn)(int);
typedef void (*release_fn)(const void*);
typedef const char* (*data_fn)(const void*);
typedef size_t (*length_fn)(const void*);
typedef int (*int_fn)(int);
typedef int64_t (*used_fn)(void);
typedef void (*deadline_fn)(int64_t);
typedef const AetherLibMeta* (*meta_fn)(void);
typedef int (*outcome_fn)(void);
typedef const char* (*outcome_text_fn)(void);
typedef int (*check_fn)(const char**);

#define SPIN_N 200000000

static void* need(void* h, const char* n) {
    void* p = lib_sym(h, n);
    if (!p) { printf("missing %s\n", n); fflush(stdout); exit(2); }
    return p;
}

/* The library's recorded host ABI level: none (level 0) before 1.11. A
 * host built with the headers of level 0 does not know the field (-1). */
static int recorded_level(const AetherLibMeta* m) {
#if HEADER_LEVEL >= 1
    int minor = 0;
    if (strncmp(m->schema_version, "1.", 2) == 0) minor = atoi(m->schema_version + 2);
    return minor >= 11 ? m->host_abi : 0;
#else
    (void)m;
    return -1;
#endif
}

/* What spin(SPIN_N) returns when nothing cuts it. */
static int spin_full(void) {
    uint64_t n = SPIN_N;
    return (int)(((n * (n - 1)) / 2) % 1000003u);
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: host <library> [its shared runtime]\n"); return 2; }
    /* The shared runtime first, so the library binds to this one (a loaded
     * image of its name), and the runtime's functions are in it; otherwise
     * they are the library's own copy. */
    void* rt = argc > 2 ? lib_open_global(argv[2]) : NULL;
    if (argc > 2 && !rt) { lib_why(); printf("cannot load the runtime %s\n", argv[2]); return 2; }
    void* h = lib_open(argv[1]);
    if (!h) { lib_why(); printf("cannot load the library\n"); return 2; }
    if (!rt) rt = h;

    const AetherLibMeta* m = ((meta_fn)need(h, "aether_lib_meta"))();
    int level = recorded_level(m);
    /* Query the actual library's ABI, including layout flags, rather than
     * assuming a downloaded release differs from this tree. Only the new
     * host uses this mode; level-0 catalogs have no runtime_abi field. */
    if (argc > 3 && strcmp(argv[3], "--runtime-abi") == 0) {
#if HEADER_LEVEL >= 1
        if (level >= 1 && m->runtime_abi && *m->runtime_abi) {
            puts(m->runtime_abi);
            return lib_close(h) != 0;
        }
#endif
        return 2;
    }
    if (level >= 0) printf("catalog: schema %s, host ABI level %d\n", m->schema_version, level);
    else printf("catalog: schema %s, read by a level-0 host\n", m->schema_version);

#if HEADER_LEVEL >= 1
    if (level >= 1) {
        check_fn check = (check_fn)need(h, "aether_mlib_check");
        const char* why = NULL;
        if (check(&why) != 0) {
            printf("preflight: refused, %s\n", why ? why : "(no reason)");
            fflush(stdout);
            return 0;
        }
        printf("preflight: ok\n");
    }
#endif

    add_fn add = (add_fn)need(h, "aether_add");
    scale_fn scale = (scale_fn)need(h, "aether_scale");
    wide_fn wide = (wide_fn)need(h, "aether_wide");
    even_fn even = (even_fn)need(h, "aether_even");
    int a = add(2, 3);
    double sc = scale(2.5, 3.0);
    long long w = (long long)wide(3000);
    int e1 = even(4), e2 = even(7);
    printf("typed: add %d, scale %.2f, wide %lld, even %d %d\n", a, sc, w, e1, e2);

    greet_fn greet = (greet_fn)need(h, "aether_greet");
    release_fn release = (release_fn)need(h, m->string_release);
    data_fn data = (data_fn)need(rt, "aether_string_data");
    length_fn length = (length_fn)need(rt, "aether_string_length");
    used_fn used = (used_fn)need(rt, "aether_caps_used_bytes");
    int64_t base = used();
    const void* s = greet(42);
    int64_t holding = used();
    printf("strings: \"%.*s\", held %s", (int)length(s), data(s), holding > base ? "yes" : "no");
    release(s);
    release(NULL);
    for (int i = 0; i < 20000; i++) release(greet(i));
    printf(", 20000 more released, the runtime holds %lld bytes more\n", (long long)(used() - base));

    int_fn checked = (int_fn)need(h, "aether_checked");
    int_fn spin = (int_fn)need(h, "aether_spin");
    if (level >= 1) {
        outcome_fn outcome = (outcome_fn)need(rt, "aether_last_outcome");
        outcome_text_fn text = (outcome_text_fn)need(rt, "aether_last_outcome_text");
        int ok = checked(3);
        int okc = outcome();
        int bad = checked(-1);
        int badc = outcome();
        char why[64];
        snprintf(why, sizeof(why), "%s", text());
        int again = checked(5);
        printf("errors: checked(3) %d outcome %d; checked(-1) %d outcome %d \"%s\"; then %d\n",
               ok, okc, bad, badc, why, again);
        deadline_fn set_deadline = (deadline_fn)need(rt, "aether_set_call_deadline");
        set_deadline(50);
        int r = spin(SPIN_N);
        int rc = outcome();
        set_deadline(0);
        int after = spin(10);
        printf("deadline: 50 ms ends a loop of 2e8: %d outcome %d, then %d\n", r, rc, after);
    } else if (level == 0) {
        printf("errors: level 0 has no outcomes; checked(3) %d\n", checked(3));
        printf("deadline: level 0, not asked; spin(10) %d\n", spin(10));
    } else {
        printf("errors: this header has no outcomes; checked(3) %d\n", checked(3));
        deadline_fn set_deadline = (deadline_fn)lib_sym(rt, "aether_set_call_deadline");
        if (set_deadline) {
            set_deadline(50);
            int r = spin(SPIN_N);
            set_deadline(0);
            int after = spin(10);
            printf("deadline: 50 ms cuts a loop of 2e8: %s, then %d\n", r != spin_full() ? "yes" : "no", after);
        } else {
            printf("deadline: the library exports no host API\n");
        }
    }

    printf("unload: %d\n", lib_close(h));
    fflush(stdout);
    return 0;
}
