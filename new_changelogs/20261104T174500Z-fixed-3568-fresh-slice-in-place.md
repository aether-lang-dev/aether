- **A slice a call hands over is given back where it is used in place
  (#3568).** `f().len`, `f()[0]`, `"${f().len}"`, `f()[1..].len`, a `for`
  over `f()`, a `while` condition and a call written as a statement of its
  own never gave back the reference a call's slice result holds, so each
  such use leaked the buffer. The statement now holds the result in a
  temporary its block gives back on every exit, a panic's unwinding
  included. A view of the result that is bound, returned or passed on still
  takes that reference over. An `append` that panics (a view that may not
  grow, or no memory) gives back the slice it was handed, which the panic
  lost. An empty `make` buffer takes one byte past its header, so its
  `.ptr` points into the block and a leak checker that follows the
  pointers a program holds (LeakSanitizer) sees the module `var` or the
  row that holds it.
