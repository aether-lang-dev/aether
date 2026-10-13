- **A type name that names nothing is an Aether error, not C.**
  `while i < 2 and eat(i) { ... }` passed aetherc and failed in the C
  compiler ("unknown type name 'and'"): Aether's AND is `&&`, so
  `and eat` read as a declaration of `eat` of a type `and`. Any name that
  is no type of the program did the same as a local's, a parameter's or a
  field's type. The type checker refuses it (E0302), and for `and` and
  `not` the help names `&&` and `!` (#2896).
