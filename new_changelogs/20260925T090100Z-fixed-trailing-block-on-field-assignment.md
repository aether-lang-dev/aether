- **A trailing block on the right of a struct-field assignment runs.**
  `obj.field = f(args) { ... }` parses as an expression statement around a
  binary `=`, not as an assignment statement, and that path emitted the
  assignment and never visited the block: the whole body vanished with no
  diagnostic. The same call assigned to a local always ran its block. In
  aether-ui this silently emptied every pane built as
  `st.pane = vstack() { ... }`. The shape now goes through the assignment
  path, builder and regular pattern alike. New test:
  `tests/regression/test_trailing_block_field_assign.ae`.
