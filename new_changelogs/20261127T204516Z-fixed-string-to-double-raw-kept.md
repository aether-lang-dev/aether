- **`string.to_double_raw` is back, as `to_float_raw` under its old name.**
  0.948.0 made `to_float` the correctly rounded 64-bit parse and kept
  `to_double`, `try_double` and `get_double` for the code still calling
  them, but removed `to_double_raw`, so a program calling it failed to
  link (`_string_to_double_raw` undefined). It fills the same 8-byte
  `float` slot as `to_float_raw` and returns the same 1 or 0. Like the
  other old names, it is to be deleted once nothing calls it; new code
  uses `to_float_raw`.
