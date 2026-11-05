- **A `match` a slice store takes holds no GNU statement expression on the
  non-GCC path (#3568).** `ys = match ...`, `h.data = match ...` and
  `rows[1] = match ...` nested the `match` in the store's call, where the C
  for a compiler without GCC's extensions (`AETHER_GCC_COMPAT` 0, what MSVC
  compiles) held a statement expression it refuses. The `match` now runs
  ahead of its statement, its value in a temporary the store reads.
