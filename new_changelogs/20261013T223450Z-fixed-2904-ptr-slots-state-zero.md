- **Closures into `ptr` locals and state, zeroed state, `ptr` fields from
  strings.** A closure stored into a `ptr` local (as a rebinding or as its
  initializer) or into a `ptr` state field did not compile; it is now
  boxed there, and a bare function stored as its code address, as in every
  other `ptr` slot. A `state fn kept` (or a struct, optional, tuple or sum
  state field) with no initializer was set with `= 0`, which C refuses for
  those; it is now zeroed whole. A struct literal whose `ptr` field took a
  `ptr` local holding a string named a `_heap_<field>` tracker the struct
  does not have; a `ptr` field takes the pointer, which its source keeps
  (#2904).
