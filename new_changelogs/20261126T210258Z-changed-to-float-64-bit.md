- **`string.to_float` parses to 64 bits, correctly rounded; `to_double`
  stays for now as the same function, slated for deletion.** Aether's `float`
  is 64-bit (C `double`), but `to_float` parsed with `strtof` and widened the
  32-bit result, so it returned about seven significant digits dressed as a
  double: `to_float("0.1")` gave 0.10000000149011612 and
  `to_float("16777217")` gave 16777216. It now parses with `strtod`,
  correctly rounded to binary64, with the syntax `to_double` accepted
  (hexadecimal constants, `inf`/`nan` spellings, the same errors), and
  `from_double`'s text reads back to the same bits; `string_to_float_raw` now
  writes a `double`. `to_double` (and the externs `string_try_double` /
  `string_get_double`) now call the same parse; they are kept until nothing
  in the aether-lang-dev org's repos or in friends-and-family repos uses
  them, then deleted, so move callers to `to_float`. The extern
  `string_to_double_raw` is removed. The new
  `string.to_f32(s) -> (f32, string)` is the 32-bit parse: `strtof`, rounded
  once, returning a real `f32`, with the f32 range's overflow an error
  (externs `string_to_f32_raw`, `string_try_f32`, `string_get_f32`).
