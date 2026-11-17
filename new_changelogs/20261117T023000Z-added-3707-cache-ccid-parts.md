- **`ae build -v` prints each part of the C compiler's identity in the cache
  key (#3707):** the driver's path and content hash, and the exit code and
  first line of its `--version`, so a cache miss traced to a moving compiler
  identity shows which part moved.
