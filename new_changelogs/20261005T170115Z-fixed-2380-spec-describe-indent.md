- **`std.spec` indents sibling `describe` blocks at the same level (#2380).**
  `describe` incremented an indentation counter that nothing decremented, so
  each sibling `describe` printed one level deeper than the one before, and a
  test written after a nested `describe` printed at the nested level. The
  depth is now derived from the suite's parent chain. Output only: pass/fail
  counts, exit status and the structured report were never affected.
