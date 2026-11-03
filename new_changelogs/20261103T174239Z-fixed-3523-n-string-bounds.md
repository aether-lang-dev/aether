- **The length-aware string functions no longer read past the end of a
  string given a longer length (#3523).** `string.substring_n`, `char_at_n`
  and `index_of_from_n`, `strbuilder.append_n` and `bytes.copy_from_string`
  took the caller's length on trust even when the string carried its own,
  so `bytes.copy_from_string(b, 0, "abc", 4096)` copied 4096 bytes out of a
  three byte string. A heap or counted string's length is exact, so the
  length given is now cut to it, and embedded NULs inside it still count; a
  plain C string handed to C directly keeps the length it is given.
  `strbuilder.append_n` now passes its string to C with its header, so the C
  side can see that length. `string.length` of a string over 2 GiB panics
  instead of returning a negative length.
