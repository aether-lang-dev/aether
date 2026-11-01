- **A local bound to one byte of a string is a `byte`** (#3479). `c = s[i]` (a
  local, parameter, field or returned string) left the local untyped until
  codegen defaulted it to `int`, with "unresolved type in codegen, defaulting
  to int", which fails `make check-examples`. The read is a byte, so the local
  is one, as a local bound to a `byte[]` element is: a high byte reads 195
  (not -61), and arithmetic stored back into the local wraps (`c = s[0]; c =
  c - 100` is 244 for "X"), where the int default gave -12. Widen with `as
  int` (`s[0] as int - 100`) to keep a signed result.
