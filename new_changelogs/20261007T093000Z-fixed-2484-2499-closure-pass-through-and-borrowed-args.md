- **A closure that returns another closure's call result is typed by where
  its result is used (#2484).** `g = || { return call(f, x) }`, with `f` an
  erased `fn`, returned the int an erased call defaults to: its C function
  returned `int`, so `let r: ptr = call(g)` cut the pointer to 32 bits under
  `ae run`, and `return call(g)` from a `-> ptr` function did not compile. A
  typed binding or a declared return of such a call now types the closure's
  return too, through any number of pass-through closures. A closure that no
  typed use reaches (one handed to an extern, or returned through `-> fn`)
  is still int, and the compiler now warns at its `return` and names the
  fix. `worker.map` built exactly that closure; it binds the result as
  `ptr` now. `docs/closures-and-lifetimes.md` showed `ptr p = call(y, 5)`,
  which is refused; it shows `let p: ptr = call(y, 5)`.
- **An owned string passed to a closure through an `fn` parameter is freed
  after the call (#2499).** `apply(f: fn, s: string) { call(f, s) }` has no
  closure body to read, so passing `s` on counted as keeping it, and every
  `apply(g, mk("x"))`, like every `call(f, mk(a))` inside such a function,
  leaked the string. Closure calls now borrow their arguments when no closure
  the program can call keeps a parameter that can hold a caller's string: a
  `string` parameter is not kept by a nested closure that captures it (the
  closure takes its own reference) or by being returned (a string closure
  returns a copy), while a store keeps it, and a `ptr` parameter is kept by a
  capture or a return as well. One keeping closure anywhere, a library
  build, or an extern that hands a closure in leaves the arguments alone as
  before. A struct literal returned with a parameter in a field now counts as
  keeping it; the caller freed that argument and the returned field pointed
  at freed memory. A string parameter a nested closure writes is copied into
  its cell when it is not refcounted, instead of being held by reference.
