- **Float comparisons keep IEEE's answer beside a NaN under GCC at -O1 and
  up (#3848).** GCC 12 and later record the false edge of `a > n` as the
  relation `a <= n`, which a NaN breaks. They chain such relations and fold a
  later comparison wrong: from `a > n` and `n > b` both false, with n a NaN,
  `a > b` became false for a = 1, b = 0, and std.lanes' pick_max returned the
  min. `ae` now passes `--param relation-block-limit=0` to every optimising
  GCC compile, a user-named inlining budget or not, and the Makefile builds
  the runtime's own C with it. With no relations recorded there is nothing to
  chain. It measured within noise on bench-core. Clang folds no such chain,
  and a GCC without the parameter predates the optimiser.
