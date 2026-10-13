- **Closures a `try` body binds hold after the catch without statement
  expressions.** With `AETHER_GCC_COMPAT` 0 (MSVC, emscripten, a C11
  compiler), a capturing closure literal was a call returning the closure
  struct through a hidden slot. A local a `try` body writes is volatile, and
  gcc 16 at -O1 and -O2 inlined that call into the local and dropped the
  store, so once the panic was caught the local, an `fn[N]` or one of its
  elements, still held the closures from before the `try`. The literal is
  now a compound literal around the environment its constructor returns,
  as it is on the GCC path (#2889).
