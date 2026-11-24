- **std.math's scalar functions are inline (#3852).** `math.sqrt`,
  `abs`, `min`, `max`, `clamp`, `floor`, `ceil`, `round` and their f32
  forms were calls into libaether.a the C compiler could not see through,
  each calling libm in turn. They now come from `aether_math_inline.h`
  through `@c_include`, as std.string's reads do (#3836), using the
  compiler's builtins on GCC and clang (no `<math.h>` in the program) and
  libm's names elsewhere; the exported `math_*` functions are the same
  bodies, and the values are unchanged (NaN handling of min/max,
  `abs_int(INT_MIN)`, half-away rounding: `test_math_inline_scalars`). A
  loop of sqrt, floor, min and abs went from 2.38 to 1.98 ns an iteration.
  Transcendental functions stay out of line. On x86-64 MinGW `math.sqrt`
  is now `sqrtsd`, correctly rounded, where the CRT's x87 sqrt rounded
  twice (#3027): a result there may differ from before by one ulp, toward
  the correct one.
