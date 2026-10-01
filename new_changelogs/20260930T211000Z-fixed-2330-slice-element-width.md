- **A slice reads its elements at their own width.** An array was accepted
  in a `U[]` slot whenever its element type was merely compatible with `U`,
  so an `int[3]` passed as a `long[]` was read as three 8-byte longs out of
  12 bytes, and as a `byte[]` as the bytes of its first int; even
  `g([1, 2, 3])` into a `long[]` parameter was built as `int[]`. An array
  literal in a slice slot is now built with the slot's element type, and any
  other array or slice must have exactly that element type or it is a
  compile error naming both. A tuple return type may now hold a slice
  (`-> (long[], string)`), and `return null, "e"` into it gives the empty
  slice (#2330).
