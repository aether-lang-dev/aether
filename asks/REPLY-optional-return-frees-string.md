# REPLY: returning a local `T?` whose `T` owns a string

**Re:** nib-code `ae/term/asks/optional-return-frees-string.md` (branch
`aether-port`)

**RESOLVED** in branch `fix/batch-2026-11-12b` (the first release after
0.909.0 carries it).

The cause was as the ask read it: an optional result was not one of the
results the return takes its value for (`emit_composite_passthrough_return`
took tuples and sums only), so `return k` / `return e` copied the local
bit for bit and the local's scope-exit drop then released the string the
copy shared. An optional result is now taken as a tuple or sum result is:
a struct local is moved into the wrapper (`_aether_move_<Name>`), an
optional local is retained before its drop.

Every shape in the ask works: `k` wrapped by the return, a `let e: Key?`
local, `e = mk()` after an early `return none`, `some(v)` re-wrapped in a
match arm, inside a loop, and a sum (`Event?`). Three siblings went with
it, all in `tests/regression/test_optional_owning_local_return.ae`:

- `return if c { k } else { none }` emitted `.val = (c ? k : {0})` and did
  not compile; an `if` into an optional is taken arm by arm now;
- `return match n { 0 -> none  _ -> k }` declared its temporary
  uninitialised, and the first arm's store dropped garbage; it is `{0}`
  and dropped, as a tuple's is;
- a call statement that throws an owning optional (or sum) away dropped
  nothing; with the result handed over properly that leaked on every
  such call, so it is dropped.

`ae/term` can go back to `Event?` and `Cell?` once it is on the release
carrying this.
