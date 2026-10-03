- **`std.mem` hot-loop operations inline without library calls.**
  `bits_of_float`, `float_from_bits`, `clz32` and `clz64` now lower inline.
  Native-endian scalar getters and setters have `_unchecked` companions
  that omit the null check when callers guarantee valid, non-null storage;
  checked accessors keep their existing behavior. `--audit-mem` includes
  the new accessors (#2379).
