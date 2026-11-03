/* The canaries of `make test-asan-ae` (#3539, tests/scripts/sanitizer_sweep.sh).
 *
 * Errors every AddressSanitizer build reports: a write one byte past a heap
 * block, and a block nothing points to when the program exits (LeakSanitizer).
 * The sizes come from the caller in another translation unit, so no compiler
 * folds them away. A configuration whose flags never reached the programs
 * `ae build` makes would let both run clean, and the run fails on that instead
 * of reporting a corpus it never checked. */
#include <stdlib.h>
#include <string.h>

int asan_canary_overflow(int n) {
    char* p = malloc(8);
    if (!p) return -1;
    memset(p, 1, (size_t)n);
    int first = p[0];
    free(p);
    return first;
}

int asan_canary_leak(int n) {
    char* p = malloc((size_t)n);
    if (!p) return -1;
    memset(p, 2, (size_t)n);
    return p[n - 1];
}
