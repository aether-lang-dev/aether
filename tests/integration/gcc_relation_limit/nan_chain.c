/* #3848: GCC's chained float relations, compiled at -O3 with the flags
 * `ae cflags` gives. GCC 12 and later record the false edge of `a > n` as
 * `a <= n`, which a NaN breaks, and chain such relations: from `a > n` and
 * `n > b` both false, with n a NaN, they fold `a > b` to false for a = 1,
 * b = 0. Each function must answer 3.
 *
 * chain is the reduction. joined puts the comparisons in blocks that meet
 * before the last test, so only a jump thread along the path (a > n false,
 * n > b false) could fold a > b: the backward threader's path oracle records
 * relations along a path whatever relation-block-limit says. */
#include <math.h>
#include <stdio.h>

volatile int nan_chain_sink;

__attribute__((noinline)) int chain(float a, float n, float b) {
    if (a > n) return 1;
    if (n > b) return 2;
    if (a > b) return 3;
    return 4;
}

__attribute__((noinline)) int joined(float a, float n, float b, int k) {
    int r = 0;
    if (a > n) r = 1;
    else if (n > b) r = 2;
    if (k) nan_chain_sink = 1;
    if (r == 0) {
        if (a > b) return 3;
        return 4;
    }
    return r;
}

int main(void) {
    volatile float a = 1.0f, b = 0.0f, n = NAN;
    int c = chain(a, n, b), j = joined(a, n, b, 1);
    printf("%d %d\n", c, j);
    return c == 3 && j == 3 ? 0 : 1;
}
