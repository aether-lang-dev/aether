- **Float comparisons keep IEEE's answer beside a NaN under GCC at -O1 and
  up (#3848).** GCC 12 and later record the false edge of `a > n` as the
  relation `a <= n`, which a NaN breaks. They chain such relations and fold a
  later comparison wrong: from `a > n` and `n > b` both false, with n a NaN,
  `a > b` became false for a = 1, b = 0, and std.lanes' pick_max returned the
  min. `ae` now passes `--param relation-block-limit=0` to every GCC compile
  it makes, at every -O level (a project's cflags, `AETHER_CFLAGS_EXTRA` or
  the `$AE_CC` / `$CC` command can say -O2 on a `--quick` build), a
  user-named inlining budget or not, and to a runtime or the vendored SQLite
  it compiles from source. The Makefile builds the runtime's own C with it,
  and `ae cflags` prints it when the compiler `ae` drives is a GCC that takes
  it, for C built by hand from generated C. With no relations recorded there
  is nothing to chain; the reduction answers right at -O3 with it. It
  measured within noise on bench-core. Clang folds no such chain, and a GCC
  without the parameter predates the optimiser.
