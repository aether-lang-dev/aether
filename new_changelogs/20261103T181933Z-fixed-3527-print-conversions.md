- **A value an interpolation, `print` or `println` has no conversion for is
  a compile error naming its type, and a long double or a uint64 prints as
  itself on every path (#3527).** Each print path ended its list of kinds
  with `%d`, so a kind it had not learned was read as an int: `println(x)`
  of a `longdouble` printed garbage, `print(u)` of a `uint64` and a `uint64`
  in a literal print format printed -1, and an unresolved type was printed
  through `%d` whatever it was. The int conversion is now used only for the
  kinds that are ints on purpose (`int`, `byte`, `uint8`, `uint16` and
  enums), every other kind uses its own, and one with none is refused.
