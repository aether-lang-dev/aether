- **`string.join` no longer takes a `string[]` it cannot read, and joins a
  `[...]` literal.** An array decayed to any typed pointer, so
  `string.join(xs, ",")` with `xs: string[]` type-checked against
  `join(s: *StringSeq, sep)` and the C array was walked as cons cells: one
  element printed, or a crash. An array now decays only to `ptr` or a pointer
  to its element, and a `string[]` given to a `*StringSeq` parameter is E0200
  naming the fix. A `[...]` literal of strings given to one is built as a
  sequence, as it already was for a variable or a field of that type, so
  `string.join(["a", "b", "c"], ",")` is `"a,b,c"`; and
  `string.join_strings(xs, sep)` joins a `string[]`.
