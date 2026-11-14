- **`contrib.quickjs` runs quickjs-ng 0.18.0 (was 0.17.0).** The engine's
  own changes include fixes for a use-after-free when a context runs out of
  memory and in WeakMaps with symbol values, a JSON.stringify leak, and
  asynchronous dynamic `import()`. One reaches scripts: an `InternalError`
  (out of memory, interrupted, a host function that failed without
  throwing) can no longer be caught by JavaScript. `quickjs.rethrow` with
  no exception value kept now throws the error its text names
  (`TypeError: not a function` is a catchable TypeError) rather than an
  InternalError wrapping it.
