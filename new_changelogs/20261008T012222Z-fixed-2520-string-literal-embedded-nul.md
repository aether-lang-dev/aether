- **A string literal keeps a NUL and every byte after it (#2520).** Tokens,
  AST values and the emitted C were C strings, so `"a\x00b"` and `"a\0b"`
  had length 1, plain and interpolated alike (`"a\0b ${n}"` lost the `${n}`
  too), with no diagnostic. The literal's decoded length now travels from
  the lexer through the AST to codegen; a literal holding a NUL is emitted
  once, at file scope, as a static string carrying its length, pinned so
  that it is never freed and `string.free` on it is a no-op, so
  `string.length`, `==`, map and set keys, `std.json`, `std.fs`,
  `string.concat` (and its compile-time fold of two literals), `match`
  arms, function-clause patterns and `const` strings see all of its bytes,
  and the match-arm reachability check no longer reports `"x"` as a
  duplicate of `"x\0y"`. `print` and `println` write every byte of such a
  literal, and a NUL in the text of an interpolation is a byte of the
  result. A C extern whose parameter is `string` still receives the bytes up
  to the first NUL, all a `const char*` can carry. A literal without a NUL
  is the plain C string it always was. A std module read from its compiled
  artifact keeps such a literal whole too: the `.aea` format is version 2,
  so an older artifact is parsed from source instead.
