- **`%` on floats compiles (#3577).** The checker accepted `a % b` on
  `float`, `f32` and `longdouble` values, and `x %= y` on them, but the C
  was C's `%`, which has no floating form, so the program did not build.
  It is now the remainder of the quotient truncated toward zero with the
  sign of the dividend, as Java's: C's `fmod`. `7.5 % 2.0` is `1.5`,
  `-7.5 % 2.0` is `-1.5`, and `x % 0.0` is NaN rather than a panic.
