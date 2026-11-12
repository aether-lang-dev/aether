- **Returning a local as an optional whose value owns a string no longer
  returns freed memory.** `k = Key { text: ... }; return k` from `-> Key?`
  (or `return e` for a `Key?` local, a `some(v)` re-wrapped, an `if` or
  `match` picking the local or `none`) copied the local into the result and
  then ran its scope-exit drop, which released the string the caller had
  just been handed; the next allocation reused it. The same held for an
  optional sum, tuple, closure or struct nesting an owning struct. An
  optional result is now taken as a tuple or sum result is: a struct local
  moved into it, an optional local retained before its drop, an `if` arm
  by arm (wrapped whole, its `none` arm did not compile), and a returned
  `match` result is initialised and dropped like a tuple's (it was declared
  uninitialised, and the first arm's store dropped garbage). A call
  statement that throws such an optional or an owning sum away now drops
  it (it leaked). Found by
  nib-code's `ae/term` port.
