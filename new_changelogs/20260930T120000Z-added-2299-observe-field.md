- **`std.observe` names the field that changed, and reports stores the
  compiler cannot see.** An observer registered with `observe.observe_fields`
  receives the stored field's index in declaration order,
  `|obj: ptr, field: int|`, so a replicator can send a changed-field bitmask
  and an undo log can record the one field a store touched; a nested store
  carries each level's own field to that level's observers. A store made by
  offset through `std.mem` is reported with `observe.notify_field(obj, field)`
  or `observe.notify(obj)` (field `observe.ANY_FIELD`), which run the same
  pass a compiler-emitted store runs. Plain `observe` observers are
  unchanged (#2299).
