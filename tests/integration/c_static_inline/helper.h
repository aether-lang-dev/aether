/* Stands in for the Redis-style header full of `static inline` helpers that
 * #1241 is about: no linkable symbol, so the assumption was that calling one
 * from Aether needs a hand-written C shim. */
#ifndef AETHER_TEST_HELPER_H
#define AETHER_TEST_HELPER_H

static inline int fast_double(int x) { return x * 2; }

static inline int clamp_lo(int x, int lo) { return x < lo ? lo : x; }

#endif
