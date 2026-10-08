- **Fixed-size array parameters and array-to-array assignment compile
  (#2516).** `f(xs: int[3])` and `a = b` between two `int[3]` locals
  reached the C compiler and failed there. A fixed-size array is a value:
  a parameter is the callee's own copy of the caller's elements (a write,
  a closure's too, stays in the callee) and binding an array to one that
  exists copies its elements. The lengths must agree, and a longer or
  shorter array passed to such a parameter is a type error.
- **Writing an element of a `const` array is an Aether error (#2516).**
  `TABLE[0] = v` or `TABLE[i]++` on a module constant was caught only by
  the C compiler.
- **The evaluation-order gaps left by #2478 are closed (#2516).** The
  indexes of an assignment's target are evaluated before its value
  (`arr[i++] = i` stores at the old `i`), a closure literal reads its
  captures where it stands and is ordered against the operands beside it,
  a call with named arguments evaluates them in the order written, and
  `pair(pqueue.pop(q), pqueue.pop(q))` pops in source order. A write
  inside the target of a compound assignment (`a[i++] += 1`) is refused
  rather than run twice.
