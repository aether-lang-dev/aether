- **A bare function in a `ptr` slot is its code address again.** The
  first #2890 fix boxed a bare top-level function like a closure in every
  `ptr` slot it touched, so C code calling through the slot got a box
  where it reads a function: `Sorter { cmp: int_cmp }` then
  `qsort(buf, n, sz, s.cmp)` crashed, and a bare function named only in a
  struct literal's or a message's `ptr` field did not compile (an adapter
  was named and never emitted). A bare function, a `@c_callback` one too,
  now stays a raw code pointer in a struct literal's field, a field
  store, a module variable and a message field, as the box documentation
  and unbox_closure's panic state; only a closure is boxed (#2890).
