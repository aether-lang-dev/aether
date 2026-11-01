- **`s[i]` on a string is a checked `byte` (#3479).** A local bound to it
  warned that its type was left to codegen; the read was a plain `char`, so
  a byte past 127 (any non-ASCII UTF-8 byte) was negative on x86 and
  positive on ARM; and an index past the end read past the string. It now
  has the type and the value `string.bytes(s)[i]` has, 0 to 255, and an
  index outside `0 ..< length` is the panic `string index 5 out of range for
  length 3`, which a `try` catches, as a slice index's is.
