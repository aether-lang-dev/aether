#include <stdio.h>
#include <stdint.h>
#include <math.h>

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

#define FAIL(fmt, ...) do { fprintf(stderr, "FAIL (line %d): " fmt "\n", __LINE__, ##__VA_ARGS__); return 1; } while (0)

typedef int64_t (*i64_fn)(int64_t);
typedef int32_t (*i32_fn)(int32_t);
typedef double  (*f_fn)(double, double);

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    void* h = lib_open(argv[1]);
    if (!h) FAIL("loading the library: %s", lib_error());

    i64_fn echo64 = (i64_fn)lib_sym(h, "aether_echo_long");
    i32_fn negb   = (i32_fn)lib_sym(h, "aether_negate_bool");
    f_fn   scale  = (f_fn)lib_sym(h, "aether_scale");
    if (!echo64 || !negb || !scale) FAIL("dlsym: %s", lib_error());

    /* int64 — a value that can't fit in int32 */
    int64_t big = 1LL << 40;  /* 1 099 511 627 776 */
    int64_t got = echo64(big);
    if (got != big) FAIL("echo_long(%lld) = %lld", (long long)big, (long long)got);

    /* bool — via int */
    if (negb(0) != 1) FAIL("negate_bool(0) expected 1, got %d", negb(0));
    if (negb(1) != 0) FAIL("negate_bool(1) expected 0, got %d", negb(1));
    if (negb(42) != 0) FAIL("negate_bool(42) expected 0 (truthy), got %d", negb(42));

    /* float — Aether `float` is IEEE-754 double-precision (C double).
     * The TYPE_FLOAT codegen-consistency fix (0.[current]) made every
     * emission site lower TYPE_FLOAT to C double; the previous mix of
     * `float` and `double` spellings produced silent ABI mismatches
     * across module / FFI boundaries. */
    double s = scale(3.0, 2.5);
    if (fabs(s - 7.5) > 1e-9) FAIL("scale(3.0, 2.5) = %f, expected 7.5", s);

    lib_close(h);
    printf("OK: int64, bool, float round-trip\n");
    return 0;
}
