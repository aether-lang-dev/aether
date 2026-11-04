- **Integer division by zero, the most negative value divided by -1, wide
  shifts and a float converted to an integer now give the same result on
  every target (#3524).** They went straight to C, where each is undefined:
  x86 crashed on `x / 0` and on `INT_MIN / -1`, ARM64 gave 0, a shift by 32
  or more depended on the CPU, and `1.0e30 as int` gave `INT_MIN` on x86.
  Java's rules hold now: `x / 0` and `x % 0` panic with the category
  `division_by_zero` and the source position, which a `catch` receives;
  `INT_MIN / -1` is `INT_MIN` and `INT_MIN % -1` is 0, for `int` and `long`;
  a shift count is masked to the width (`& 31`, `& 63`) and a left shift
  into the sign bit wraps; a float converted to an integer type (with `as`,
  or stored into an integer variable, field, parameter or return value)
  saturates at the type's range and NaN gives 0. The compound forms follow
  the same rules. A constant divisor other than 0 and -1, a constant count
  inside the width or a float constant inside the range still compiles to
  C's own operator. `make test-ubsan` checks `float-cast-overflow` too.
