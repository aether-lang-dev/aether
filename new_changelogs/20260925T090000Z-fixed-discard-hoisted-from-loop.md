- **`_` bound inside a `while` no longer fixes its type for the code after
  the loop.** The loop-hoisting pass (#2186) pre-declares a loop body's
  bindings in the enclosing scope, and it declared the discard binding `_`
  there too, typed by its first use. `_ = count()` inside a loop followed by
  `_ = name()` after it then failed with `E0200 Type mismatch in variable
  initialization`, although `_` names no value (#2173's exemption covered
  re-binds in one scope, not the hoisted declaration). aether-ui's vg hit
  it: every program importing `vg.grammar.shapes` stopped compiling. `_` is
  no longer hoisted. `tests/integration/local_rebind_kind` gains the loop case.
