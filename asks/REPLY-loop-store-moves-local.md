# REPLY: a local stored into slice elements inside a loop

**Re:** nib-code `ae/term/asks/loop-store-moves-local.md` (branch
`aether-port`)

**RESOLVED** in branch `fix/batch-2026-11-12b` (the first release after
0.909.0 carries it).

Semantics, as the ask proposed: a store copies (retains) there; it is not
a use-after-move error. The documented rule is that a local moves on its
last use, and a read that a loop runs again is not a last use, so moving
it was the bug, not the program.

Cause: `alias_source_must_copy` judged "last use" by counting the reads of
the name in the whole function, and one read meant the move. A function's
locals are hoisted, so they keep their value across iterations, and the
second run read what the first left: nothing. That is also why it seemed
to depend on the rest of the function: any second read of the local
anywhere turned the move off.

Now a sole read inside a `while`/`for` loop or a closure body is taken
(retained, `_aether_copy_<Name>` for a struct), unless the loop body binds
the local unconditionally ahead of the read (`for i { x = f(i); a[i] = x }`
still moves, each run having a value of its own). The same cause was
behind "an outer string assigned to a loop local is freed when the
iteration ends" in std.posix.regex's AT&T test (#3654): `pattern =
lastpattern` moved `lastpattern` out.

Both repros, plus a struct literal field, a field store through a pointer,
an inner loop, and a binding under a condition, are in
`tests/regression/test_loop_sole_read_not_moved.ae`. `ae/term` can drop
the `blank_cell()`-per-iteration workaround on the release carrying this.
