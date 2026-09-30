- **A struct with an enum-typed field compiles.** Codegen emitted every
  struct body before the enum typedefs, so `struct P { d: Dir }` named a type
  C had not seen yet ("unknown type name 'Dir'"). Enum typedefs now come
  ahead of the struct bodies; an enum depends on no other type (#2308).
