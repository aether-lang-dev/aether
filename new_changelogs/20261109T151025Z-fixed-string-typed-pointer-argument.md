- **A typed pointer or a struct passed where a `string` goes is a type
  error.** `f(key: string)` called as `f(vc)` with `vc: *Vcr` passed
  `ae check` (a binding and a `return` already refused it); the C compiler
  then saw a `Vcr*` given to a `const char*`, an error under gcc 14 but only
  a warning under clang, so on macOS the callee read the struct's bytes as
  characters. A servirtium-vcr refactor that turned ~165 functions' string
  key into `vc: *Vcr` left such call sites behind with no diagnostic. The
  argument check now refuses a `*T` or a struct value for a `string`
  parameter, and a string for a `*T` parameter, for Aether functions and
  externs alike. A bare `ptr` stays the universal pointer both ways, `null`
  still goes to a `*T`, and a @c_struct overlay pointer is still a `void*`.
