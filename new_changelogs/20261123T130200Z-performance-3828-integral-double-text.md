- **An integral float is written without `snprintf` in JSON, interpolation
  and `string.from_float` (#3828).** `json.num(5.0)` was formatted with
  `%.15g` and parsed back to check the round trip, and `"${x}"` and
  `string.from_float` formatted with `%g`, for a value whose text is just its
  digits. A value with no fraction below 1e15 (JSON) or 1e6 (`%g`) now
  writes its digits directly, "-0" for -0.0; every other value takes the
  old path, so the text is the same byte for byte. `json_encode_5000_keys`:
  593 to 105 us; the new `interp_float_integral` row: 149 to 47 ns.
