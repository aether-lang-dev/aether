- **A capturing closure stored into a `ptr` field outlives the call that
  stored it.** `e.cb = h` with a `ptr` field boxes the closure, and the box
  took no reference of its own to the closure's environment, while the
  escape walk counted the store as the holder's keep (#2528) and the caller
  released its own reference straight after the call. The environment was
  freed while the field still pointed at it: a handler invoked later read
  freed memory (a segfault on glibc, garbage or a crash on macOS). 0.796's
  alias rule (#2670/#2671) made aether-ui's workaround (`kept = h; e.cb =
  kept`) fail the same way, so every vg click handler with captures
  crashed. The box now takes a reference, as a `fn` field does (#2525): a
  fresh closure's is adopted, a parameter's, local's or alias's is retained.
  The same store of a closure local freed at the storing function's end was
  the same bug (aether-ui asks/aether-closure-drain-through-fn-store.md).
