- **A closure in a loop body writes the body's variable.** `c = 0; f = || {
  c = 5 }; f()` inside a `while`, a `for`, a branch or a `match` arm left `c`
  at 0: the closure's assignment found only the function's top-level
  declarations, so it made a fresh `c` of its own. It now writes the binding
  visible from it, declared before it in any block on the way down to it,
  shared through a cell: one across a `while`'s passes, made and released
  with each pass of a `for` body or a branch, and kept alive by a closure
  that outlives its pass. A binding in a sibling block is still another
  variable (#2659).
- **A `return` in a closure's body is the closure's.** A string cell one
  closure writes and another returns (`read = || { return s }`) was taken
  for a local the enclosing function returns, so the function freed it at
  its own returns as a plain string: the C did not compile, a `const char**`
  passed as a string, or, for a cell in a `for` body, a name out of scope
  (#2667).
