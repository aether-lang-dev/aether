- **A closure's capture shadows a top-level function of the same name, as
  a parameter or local does outside a closure.** A closure that captured
  its enclosing function's `cb` and passed it on as a value
  (`run_it(|n| { call2(cb, n, x) })`) passed the program's top-level `cb`
  instead, with the caller's closure and its env dropped: wrong output, a
  SIGSEGV through the adapter's mismatched signature (aether-ui's
  `reorderable(cb: fn ...)` in a program with its own `cb`, across the
  module boundary), or C that did not compile where the adapter was never
  declared. Captured params and locals alike, in nested closures and
  `callback { }` blocks, passed as an argument, bound to a local, boxed,
  returned, or stored in a `fn` field. The closure body's prologue declared
  each read-only capture but never registered it as a binding of the body,
  so codegen's "is this a top-level function?" checks, which defer to a
  local binding first (#1657), found none. They are registered now, and a
  capture named like a function is retained and released as one of any
  other name is: the C is the same.
  `tests/regression/test_closure_capture_shadows_function.ae`,
  `tests/integration/closure_capture_shadows_consumer_fn`.
