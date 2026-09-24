- **A string stored into an `extern struct ... @c_import`'s string field
  compiles, and borrows.** The store was treated like one into an
  Aether-defined struct and also wrote a `_heap_<field>` ownership flag the C
  header does not have, so the generated C did not compile; a struct literal
  of such a type got the same trackers, and a local holding one was declared
  with the bare tag, which needs a typedef the header need not ship. The field
  now borrows the string, as the C API that declares it expects, and the local
  is declared `struct Name`. A string the function made and stored in such a
  struct, by a field store or in a literal, is kept alive rather than freed at
  the function's exit, since the struct may outlive it
  (docs/c-interop.md, "String fields of a header-defined struct borrow").
