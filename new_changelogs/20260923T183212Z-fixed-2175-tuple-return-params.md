- **A multi-value `return` of a parameter is typed, not defaulted to `int`
  (#2175).** Constraint collection visited only the first value of a
  `return`, so in `return lo, hi` only `lo` was ever typed. A local in a
  later slot was rescued by reading its declaration back; a parameter has no
  declaration in the body, so its slot stayed unknown. Codegen then printed
  `unresolved type in codegen, defaulting to int` at the function, at its
  `return` and at every destructuring caller: 16 warnings in every program
  that imported `std.cryptography.des3` and 19 for `std.cryptography.aes`
  users, all pointing into the standard library. The fallback was not only
  noise, either: a `string` or `long` parameter in that slot was typed
  `int`. Every returned value is now collected.
  `tests/integration/tuple_return_params` covers parameters of three types,
  directly and through an import, plus a `des3` program, and fails on the
  warning.

  `std.schema`'s `refine` rule also stopped warning in every program that
  used it: it now declares the closure's `int` result
  (`int res = call(pred, v)`), as the compiler's own diagnostic asked.
