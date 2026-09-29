- **A `while` loop the compiler collapses into a closed form now ends where
  the loop does (#2271).** The arithmetic-series collapse set the counter to
  the bound, which is right only for an integer step that divides the
  distance. `while a < 0.0 { a = a + 6.28 }` from -0.51 left `0.0`, and an
  integer stepping by 2 from -3 stopped at 0 instead of 1. It also truncated
  float counters, bounds and addends through `(int64_t)`, and summed an
  accumulator placed after the increment from the counter's old value. Only
  integer loops are collapsed now, with the exact trip count, any positive
  step, and the statement order respected. The arithmetic is exact modulo
  2^64, so a wrapping `int` accumulator ends where the loop leaves it. A
  float series runs as a loop, and so does an integer loop whose counter
  would wrap on its last step.
