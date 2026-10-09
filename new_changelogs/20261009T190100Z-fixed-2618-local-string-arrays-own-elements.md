- **A local `string[N]` array owns its elements.** A fresh string stored
  into one leaked, and a view stored into one (`arr[0] =
  make_item(w).tag`) dangled once the statement's temporary was gone. An
  array some store into which is not a literal now owns its elements as a
  string array cell does: a store takes the value and frees the one it
  replaces, `arr = [arr[1], arr[0]]` takes both before storing either, the
  scope exit frees what is left, and `string.free(arr[i])` empties the
  element. An element of such an array, of a `string[N]` parameter, field
  or state field is copied where it is kept: bound to a local, returned
  (directly, in an `if` or `match` arm, as a tuple position), handed back
  by a call, or copied into a closure's environment. A `string[N]`
  parameter that stores into itself takes its own reference to each of its
  caller's elements. A table of literals stays a plain C array. Also fixed
  on the way: a struct a call returns, stored from as `o.f =
  make_item(w).tag`, leaked; a pointer local or array a `try` body writes
  was declared `volatile const char*`, which left the pointer itself
  unprotected across the panic; and an array declared from another
  (`int[3] b = a`) did not compile (#2618).
