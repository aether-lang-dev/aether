- **A string stored into a struct field through a pointer from a call, a
  pointer field or a cast frees the field's previous string (#2369).** Only
  a local bound to `heap.new` in the same function released the old value,
  because the release reads the box's ownership tracker, and a box made with
  `malloc(n) as *T` has garbage there (#1873). A box returned by a
  constructor, held in another struct's pointer field, or passed as a `ptr`
  and cast back leaked every string it replaced. The compiler now follows
  the pointer back to where it was made: a function every return of which is
  a `heap.new` box, a struct field every store into which is one, a local
  every binding of which is one, or a cast of one. A pointer whose origin it
  cannot see (a parameter, a list element, a C function's result) still only
  sets the tracker, as before. Code that freed the old value by hand before
  storing through one of the pointers now covered frees it twice; drop that.
- **`ae build` and `ae run` rebuild when the C compiler changes (#2477).**
  The cache key covered the source, aetherc, ae, libaether and the flags, but
  not the C compiler, so the same source built with another `gcc` first on
  PATH, another `$CC` / `$AE_CC`, or a compiler upgraded in place was handed
  the binary the previous compiler made, reported as a cache hit. The key now
  includes the compiler setting and, for each program it names, the resolved
  path and a hash of the file.
