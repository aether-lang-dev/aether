- **A value an interpolation, `print` or `println` has no conversion for is
  a compile error naming its type, and a long double or a uint64 prints as
  itself on every path (#3527).** Each print path ended its list of kinds
  with `%d`, so a kind it had not learned was read as an int: `println(x)`
  of a `longdouble` printed garbage, and `print(u)` of a `uint64` and a
  `uint64` in a literal print format printed -1. The int conversion is now
  used only for the kinds that are ints on purpose (`int`, `byte`, `uint8`,
  `uint16` and enums) and for a type the checker leaves unresolved, which
  the compiler stores as an `int`; every other kind uses its own, and a
  value with no type at all is refused. An element read through a typed
  pointer (`p[0]` on a `*T`) has its element's type now, where it was left
  unresolved.
- **Migrating: printing an actor ref, a bit set, a bitstruct or an optional
  is a compile error (#3527).** `"${ref}"` or `print(flags)` of these kinds
  went through `%d` and printed whatever the int read of the value gave.
  Print what you mean: an actor's id or a field of its state, a bit set's
  members, a bitstruct's fields, or an optional unwrapped (`if v != none`).
