- **A store into a struct field frees the value it replaces through any
  pointer.** Only a pointer the compiler could trace back to `heap.new`
  released the old string; through a list element, a call returning one, a
  cast of a `ptr` parameter or a parameter of a function C may call, every
  replaced string leaked, and so did a replaced struct field's strings and
  a replaced closure field's environment. The store reads the field's
  `_heap_<field>` tracker wherever the pointer came from: a call of C's
  `malloc` from Aether is now a zeroing allocation (`calloc`), as `heap.new`
  is, whether its pointer is cast to a struct at once or later, so the
  tracker of a box Aether allocated is never garbage. A field freed by hand
  before the store (`string.free(p.name)`, or `p.name = set_owned(p.name,
  v)` with a `set_owned` that frees its first argument) is given up at the
  free and not freed twice (#2369).
