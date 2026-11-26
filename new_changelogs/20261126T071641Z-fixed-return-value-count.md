- **A multi-value `return` with a different count than the function's result
  is the checker's error.** A function whose first `return` gives one value
  and a later one two (`return 0` ... `return v, 1`), or two and then three,
  or three and then two, was let through and lowered to the first
  `return`'s shape: a tuple returned into an int result reached the C
  compiler, a longer tuple silently lost its extra values (a C "excess
  elements" warning only), and a shorter one left the rest zero. Each is now
  E0200 at the `return`, naming the result type and how many values the
  `return` gives; the same for a written result type (`-> (int, string)`
  returning three values).
