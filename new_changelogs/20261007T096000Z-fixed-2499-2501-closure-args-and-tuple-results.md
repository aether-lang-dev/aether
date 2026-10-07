- **A closure-call argument is no longer freed when a closure from outside
  the program can be called (#2499).** The borrowed-arguments convention let
  a caller free its owned string after any call through an `fn` parameter,
  on the strength of having checked every closure in the program. A closure
  can also come from outside it: a box recovered with `unbox_closure` or by
  passing a `ptr` to an `fn` parameter, a closure an extern returns, one C
  passes to a `@c_callback`, one stored in a struct C fills or lays out, or,
  in a library build, whatever the host passes. Such a closure may keep its
  argument, and the caller freed it under it. The convention is now off in
  every one of those cases.
- **A closure that keeps a `string` parameter takes its own reference to
  it (#2499).** A closure that stores its `string` parameter (in a list, a
  map, a struct field, a captured variable) turned the borrowed-arguments
  convention off for the whole program, so its callers leaked what they
  passed. Such a closure now retains (or, for a plain buffer, copies) the
  string when it is entered, and a function used as a closure value gets the
  same from its adapter, so callers free their argument after the call. Only
  a closure that keeps a `ptr` parameter, which cannot be copied, still turns
  the convention off. docs/memory-management.md describes the convention.
- **A closure that returns several values has a tuple result (#2501).**
  `f = |n: int| { return "v", n }` was typed by its first value, so
  `s, k = call(f, 4)` was refused ("'call' returns 'string', not a tuple"),
  and a call left whole did not compile (a `_tuple_string_int` returned
  where `const char*` was declared). A multi-value return now gives the
  closure a tuple result type in the type checker and in its C function,
  with the tuple's typedef, and each string slot is handed over owned, as a
  string closure's result is, so the destructured binding frees it.
