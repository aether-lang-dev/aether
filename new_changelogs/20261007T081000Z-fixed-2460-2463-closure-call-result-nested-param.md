- **Calling a closure literal bound without a type yields what its body
  returns (#2460).** A closure literal's type is the erased `fn`, so
  `r = call(f, x)` (and `r = f(x)`) on `f = |t: string| -> t` was typed int:
  `println(r)` printed the string's address as a number, a float result
  printed 0, and the compiler warned the result was "assumed int". The call
  now takes its type from the literal the variable holds, also through an
  alias, a capture and a closure that returns a closure, by the same rule
  that picks the closure's C return type; a string result is owned and freed
  by the binding. A variable re-bound to a closure with a different result
  type goes back to the erased `fn`.
- **A closure nested in another closure can write the outer closure's
  parameter (#2463).** `outer = |p: int| { inner = || { p = p + 1 } ... }`
  failed in the C compiler ("makes pointer from integer"): the parameter
  stayed a plain value while the nested closure expected a shared cell. The
  outer closure now keeps such a parameter in a cell, as a function does, so
  the write is seen after the nested call. A string parameter's cell, for a
  closure or a function, now takes its own reference to the caller's string;
  it used to free the caller's string on the first write through it or at
  scope exit. Closure bodies also no longer inherit the string-escape sets
  of the function emitted before them, which made a string closure free an
  undeclared variable when another closure returned a local of that name.
