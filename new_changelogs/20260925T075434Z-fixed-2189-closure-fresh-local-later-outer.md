- **A closure's fresh local is no longer compiled as a capture of a
  same-named binding declared later in the enclosing function (#2189).** A
  `name = expr` in a closure, where the enclosing function also binds `name`
  but *after* the closure, was promoted to a write-only capture of that later
  binding on a coincidental name match. codegen then referenced a name
  declared later — `use of undeclared identifier` in the generated C — for the
  two-levels-deep shape that aether-ui's OpenDisk port hit (`each` render
  closure with a nested button callback binding `nm`, and the enclosing
  function binding its own `nm` afterwards). The write-only capture now keys on
  **declaration order**: a closure captures an enclosing binding only when that
  binding *precedes* the closure; a same-named binding that follows is an
  unrelated later local, so the closure's `name` stays its own fresh local. A
  nested closure's reads of its own locals are likewise not treated as free
  variables of the enclosing closure. `tests/regression/test_closure_nested_fresh_local_later_outer.ae`
  covers two-level, one-level and no-later-binding shapes; it fails with the
  undeclared-C error before the fix. A separate two-level write-THROUGH
  heap-use-after-free (a captured value stored into an outer cell but owned by
  the inner closure's env) is pre-existing and tracked as a follow-up in
  `asks/closure-capture-two-level-red-reproducers.md`.
