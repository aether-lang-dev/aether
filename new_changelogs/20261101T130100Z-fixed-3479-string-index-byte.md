- **`s[i]` on a string is a checked `int` from 0 to 255 (#3479).** A local
  bound to it warned that its type was left to codegen; the read was a
  plain `char`, so a byte past 127 (any non-ASCII UTF-8 byte) was negative
  on x86 and positive on ARM; and an index past the end read past the
  string. It is now an `int` with the value `string.bytes(s)[i]` has.
  `s[length]` is the terminator, 0, as a C-style scan reads it, and an
  index below 0 or past the terminator is the panic `string index 5 out of
  range for length 3`, which a `try` catches, as a slice index's is.
