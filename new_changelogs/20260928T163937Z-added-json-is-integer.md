- **`json.is_integer(value)` tells `42` from `42.0`.** It is 1 for a number
  written as an integer within int64 (or built with `from_int`), and 0 for
  one written with a fraction or exponent, `-0`, and anything that is not
  a number. The parser already kept this. A consumer that must print `3`
  and `3.0` as they were written, as Liquid does, can now read it.
