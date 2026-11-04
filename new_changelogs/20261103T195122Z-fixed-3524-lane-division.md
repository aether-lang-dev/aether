- **Integer lane division by zero and a lane's most negative value divided
  by -1 now follow the scalar rules (#3524).** `/` and `%` on `i32x4`,
  `i64x2`, `i16x8` and `i32x8` went straight to C's vector operator, where
  both are undefined, so a zero lane in the divisor stopped the program on
  x86. A zero in any lane of the divisor now panics with the category
  `division_by_zero` and the source position, and the most negative value
  divided by -1 is itself, its remainder 0, lane by lane, with a lane value
  or a scalar on either side and for `/=` and `%=`. A constant scalar
  divisor other than 0 and -1 still compiles to C's vector operator.
