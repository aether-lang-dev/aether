- **`offsetof(T, f)` in a module keeps `f` a member name.** The module
  merge renamed a module's references to its own functions and constants,
  and walked into `offsetof`'s field operand as well, so a module with a
  getter named like a field (`strength(t)` beside `strength: float`) emitted
  `offsetof(struct Thing, shade_strength)` and failed to compile. The field
  operand is never renamed now (#2320).
