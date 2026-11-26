- **`-> f32! { ... }` declares a fallible function, as `-> float! { ... }`
  does.** A result type whose payload is a scalar spelt as a name (`f32`,
  `int64`, `uint8`, `uint16`, `uint32`, `size_t`, `int32_t`, `longdouble`)
  or a struct (`-> Point! { ... }`) was read as the start of an expression
  body, so the `{` after it was a top-level parse error. Only the type
  keywords (`int`, `float`, `byte`, ...) worked.
