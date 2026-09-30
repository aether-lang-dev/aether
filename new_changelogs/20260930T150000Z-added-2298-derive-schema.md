- **`@derive(schema)`: a struct's field table at compile time, and
  `std.reflect` to read it.** The compiler emits, as static constant data,
  the struct's name and size and each field's name, type, kind, offset and
  size (C's own `offsetof`/`sizeof`), and `T_schema()` returns it. Attributes
  written after a field's type (`rate: float @range(0.0, 10.0) @default(2.0)`)
  are carried into the table. Nested structs name their own tables, a string
  field gives its ownership flag's offset for generic writers, and module
  structs work from the module and from importers. An inspector, save file or
  replicator walks a struct's fields by name instead of keeping a hand-written
  copy of the struct beside it (#2298).
