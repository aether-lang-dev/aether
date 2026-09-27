- **A library's `math.floor` resolves to std.math even when the program
  imports its own `pkg.math`.** Two modules ending in the same segment
  each got their full path as namespace, but a std or contrib module's
  functions are runtime externs compiled under the short prefix
  (`math_floor`), so `std_math.floor` found nothing and `import std.math
  as stdmath` failed the same way. A shipped module now keeps its last
  segment and only the colliding local module moves. (#2190)
