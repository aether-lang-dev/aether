- **A view or an `if` given to an optional parameter no longer frees the
  slice it views (#3568).** `count(xs[1..])` and `count(if c { xs } else {
  mk(2) })` with `count(o: int[]?)` put the slice into the optional
  argument's temporary without a reference of its own, and the temporary's
  release after the call gave back `xs`'s. The temporary takes its own now.
