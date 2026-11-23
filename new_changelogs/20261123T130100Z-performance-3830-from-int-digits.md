- **`string.from_int` and `string.from_long` write their digits directly
  (#3830).** Each formatted through `snprintf`, then measured the text again
  with a `strlen` to make the string. They now write the decimal digits into
  a stack buffer, as interpolation already did for integers, and copy them
  into a string of exactly that length. The text is the same. The new
  `string_from_int` row of benchmarks/core: 56.8 to 37.3 ns.
