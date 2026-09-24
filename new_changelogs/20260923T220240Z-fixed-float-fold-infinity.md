- **A constant float expression that overflows to infinity compiles.** The
  optimizer folded `1.0e308 * 10.0` at compile time and printed the result
  with `%.17g`, which spells infinity `inf`, so the generated C
  (`double big = inf;`) failed with "'inf' undeclared". Like a division by
  zero, a fold whose result is not finite is now left to the runtime, which
  computes the same IEEE value. `tests/regression/test_float_fold_overflow.ae`
  covers both signs.
