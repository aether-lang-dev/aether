- **`number.bytes` keeps 3 significant figures across a rounding carry, and
  renders `INT64_MIN`.** The unit and fraction digits were chosen before
  rounding, so `bytes_si(999999)` was `1000 KB`, `bytes_si(9999)` was
  `10.00 KB` and `bytes_iec(1048575)` was `1024 KiB`; they are now `1.00 MB`,
  `10.0 KB` and `1.00 MiB`. The mantissa is rounded exactly in integers, so a
  tie such as `9995` rounds half up, and `bytes_si(INT64_MIN)` is `-9.22 EB`
  instead of `--9223372036854775808 B`.
- **`std.number` rejects an exponent it cannot hold instead of wrapping it.**
  `format_decimal_string("en-US", "1e2147483648", ...)` returned `1` with no
  error. An exponent beyond ±1,000,000 is now `"exponent out of range"`, and
  the decimal point is moved in one pass rather than one digit per step.
- **`std.decimal` reports an exponent overflow as an error.** `shift` of
  `1e2147483647` by one gave exponent `-2147483648`, and `1e-2147483647`
  squared gave exponent `2`. `multiply` and `shift` now return
  `(value, err)`, as `pow` and the divisions do, with
  `"decimal exponent out of range"` when the result's exponent leaves the
  `int` range; `quo_rem`, `divide` and `divide_round` check theirs too.
- **`decimal.pow` and the bignum shifts take `INT_MIN` without overflowing
  the stack.** Each negated a negative count and recursed, and `-INT_MIN` is
  still `INT_MIN`. `bignum.shift_left(5, INT_MIN)` is now `0`, and
  `decimal.pow(2, INT_MIN)` is `0`, returned as soon as the power is sure to
  round away at the division precision.
- **JSONPath slices keep bounds and steps past 32 bits.** They were truncated
  to `int`, so on `[10,20,30]` `$[4294967296:]` selected every element,
  `$[::4294967297]` stepped by 1, and `$[1::2147483647]` wrapped into two
  null results. Slices are now applied in `long` across RFC 9535's full
  ±(2^53−1) range.
