- **A function whose result is not void must return a value on every path.**
  `f(n: int) -> string { msg = "n=${n}"; msg }` and `g(n: int) -> int { if
  n > 0 { return 1 } }` compiled, and the C fell off the end of a non-void
  function, undefined behaviour that printed `f`'s string as "(null)" on
  Windows. Control that can reach the end of a function, clause or closure
  that returns a value is now error E0700, reported at the closing brace with
  the statement that lets it through: an `if` with no `else`, a loop that can
  end, a `match` with no `_` arm that does not cover its type, a `switch`
  with no `default`, a `catch` that ends, or a last expression a `{ }` body
  does not return. A function with no written type that returns a value on
  one path is held to the same rule. An arrow body ending in an `if`, a loop
  or a `switch` no longer wraps it in a `return` (that emitted C that did not
  compile), and a C-style `for` with an empty clause, `for (;;)`, compiles:
  the optimizer and the module merge closed the empty slots, so the body was
  read as the init (#2684).
