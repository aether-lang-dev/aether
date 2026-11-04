- **Deep expressions and long `else if` chains compile with Clang
  (#3574).** The C of an expression nested about as its tree did: a call
  in a call, a cast of a cast, `!` of `!`, an interpolation in an
  interpolation, a method chain `x.f().f()...`, each one to four brackets
  deeper, and the parser takes trees far deeper than the 256 brackets
  Clang allows. `if` expressions and `if` statements with long `else if`
  chains nested a level a link too. Every 24 levels of an expression a
  part is now evaluated into a temporary ahead of its statement, in the
  order the statement evaluates it, and an `else if` chain of more than
  16 links is written flat, each condition still evaluated only when no
  branch before it was taken: the C of an expression nests by a constant,
  however deep or long.
