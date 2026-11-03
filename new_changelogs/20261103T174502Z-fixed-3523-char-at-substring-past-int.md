- **`string.char_at` and `string.substring` work on strings over 2 GiB
  (#3523).** Both compared the caller's position with the length cut to an
  int, which is negative for such a string, so every byte read as out of
  range and every substring came back empty. The length is now compared as
  it is stored; the positions are ints and always fit.
