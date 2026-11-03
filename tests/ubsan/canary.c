/* The canary of `make test-ubsan` (#2488, tests/scripts/sanitizer_sweep.sh).
 *
 * Undefined behaviour every UBSan build reports: a shift of a 32-bit int by
 * 32. The amount comes from the caller in another translation unit, so no
 * compiler folds it away. A configuration whose flags never reached the
 * programs `ae build` makes would let the canary run clean, and the run
 * fails on that instead of reporting a corpus it never checked. */
int ubsan_canary_shift(int by) {
    return 1 << by;
}
