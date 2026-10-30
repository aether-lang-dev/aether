- **`std.audit` now returns coherent entries during concurrent writes.** Its
  field accessors previously released the ring lock between category,
  resource, and verdict reads, so a wrapped ring could advance and combine
  fields from different entries. Each accessor sequence now reads one
  thread-local snapshot.
