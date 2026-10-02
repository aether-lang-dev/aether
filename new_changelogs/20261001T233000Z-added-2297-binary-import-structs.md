- **Structs cross a binary import.** An `--emit=lib` library's
  `aether_lib_meta()` catalog (schema 1.3) now carries struct records:
  each struct the library defines or its exports reach, field by field,
  with each field's type in Aether source spelling. It also carries a
  source signature for each export (`(m: *Model, p: Vec3) -> void`). The
  interface `ae` builds for an `import` of the library declares those
  structs and types each extern from its source signature. An importer can
  pass a struct by value, reach fields through a `*Struct`
  (`m.position.x`), and use a struct with a function-pointer field, all
  with the library's own layout. Before, a typed pointer arrived as an
  opaque `ptr`, and a function that took or returned a struct was not
  exported at all. Structs by value and `fn(...) -> R` function pointers
  now cross the `aether_<name>` ABI, a typed-pointer return is cast for
  GCC 14, and `ae lib-info` prints the struct records and source
  signatures. A library without them keeps its schema 1.2 catalog
  byte for byte (#2297).
