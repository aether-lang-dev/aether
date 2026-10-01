- **`byte as int` converts, and a returned value is type-checked.** A
  value cast accepted every numeric kind except `byte`, so assigning
  `bytes[i] as int` was refused with E0200. The same cast compiled inside a
  `return`, because a returned value went through the statement checker and
  no expression rule ever ran on it. A `byte` now converts with `as` like
  the `uint8` it is, in both directions (`n as byte` keeps the low 8 bits).
  A returned value is checked as an expression, the same way a
  declaration's initializer is, so an invalid cast such as `return (s as int)
  & 255` with a string `s` is now an error. Before, it compiled to a
  pointer-to-int conversion (#2337).
