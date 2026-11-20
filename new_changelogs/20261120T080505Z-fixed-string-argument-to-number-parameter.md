- **A string passed to a number parameter is refused by the checker.**
  `string.pad_start(s, 8, "0")` (the pad is a character code, an `int`, as
  the reference says) passed `ae check` and failed in the C compiler with
  "incompatible pointer to integer conversion"; so did a string given to
  an Aether function's own `int`, `float` or `bool` parameter (gcc 11 only
  warned, and passed the pointer as a number). It is now E0200 at the call,
  `Argument 3 'pad_char' of 'string.pad_start': expected int, got string`,
  and for a one-character literal the hint gives the code to pass
  (`48` for `"0"`). A string to a `ptr` parameter is unchanged.
