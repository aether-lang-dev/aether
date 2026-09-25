- **`ae run` / `ae build`'s cache now invalidates when aetherc itself
  changes, even within the same wall-clock second.** The cache key folded the
  toolchain binaries — aetherc (which owns codegen), the `ae` driver, and
  libaether — in by `st_mtime`, which is second-granularity. A `make` that
  rebuilt aetherc with a different codegen followed by an `ae run` in the same
  second produced an identical key and served the binary the *old* compiler
  emitted: a different codegen, reported as success, with every measurement
  taken against it silently wrong (the worst shape a cache bug takes, and the
  one this key's own comments warn about for source files). The three toolchain
  binaries are now keyed by a content hash (`fnv64_file`, ~1 ms each on a
  ~2 MB binary — negligible beside the recompile a real miss triggers) rather
  than mtime, so any change in what they produce is reflected regardless of
  timestamp granularity. `tests/integration/cache_compiler_invalidation`
  changes aetherc's bytes while restoring its mtime and asserts the next run
  is a miss; it fails on the pre-fix key.
