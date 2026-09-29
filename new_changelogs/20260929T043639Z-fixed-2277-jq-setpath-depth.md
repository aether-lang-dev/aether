- **contrib.jq: `setpath` and the assignment operators refuse a path past
  the 512-level value depth at once (#2277).** The depth was measured on
  the finished result, so a path far past the limit first built a value
  that deep, bottom-up, each level deep-copying the one below:
  `setpath([range(100000) | 0]; 1)` took 76 s wherever the stack let the
  walk get that far. The depth is now settled on the way down: each field
  or index step adds one container, a slice step adds none, and the value
  set brings its own depth. The same path now fails in milliseconds, with
  the same error on every stack, where a small stack used to answer "path
  nested too deeply".
  Already in 0.738.0, whose notes were cut before this merged (#2282).
