# REPLY: a closure stored through `fn -> ptr` keeps its environment

**Re:** aether-ui `asks/aether-closure-drain-through-fn-store.md`

**RESOLVED** in branch `fix/win-main-closure-drain-contrib-hooks` (the
first release after 0.799.0 carries it).

Not fixed by 0.796's ownership work; made worse by it, as aether-ui found:
#2670/#2671 give `kept = h` a reference of its own, released at the
setter's end, so the alias workaround stopped working and every vg click
handler with captures crashed (macOS too, not only glibc).

## What was wrong

Not the escape walk's verdict, in the end, but what the store did. Since
#2528 the walk counts a closure stored into a field as the holder's keep
(`closure_param_store_retains`), and the caller releases its own reference
right after the call: correct for a `fn` field, whose store retains the
environment (#2525). A `ptr` field's store boxes the closure instead
(`_aether_box_closure(h)`), and the box took no reference. So the caller's
release was the last one, and the box pointed at freed memory. A closure
local stored the same way and released at the storing function's end was
the same bug, as was the alias.

## The fix

The box takes its reference as a `fn` field's store does
(`emit_closure_take`): a fresh closure's is adopted, a parameter's, a
local's or an alias's is retained:

    e->cb = _aether_box_closure(_aether_closure_retain(h));

The env then lives as long as the box (which nothing frees, as before: a
box in a `ptr` field is the field owner's to manage).

## Tests

`tests/integration/closure_store_through_fn_coercion_no_uaf`: the direct
store, a forwarding setter, a nested field, a closure local and your alias
form, each invoked after its setter returned, plus a `fn` field, a global, a
list and a map, direct and aliased. On 0.799.0 every `ptr`-field handler
printed `(null)` for its captures on macOS. aether-ui's own
`examples/vg_image_demo` spec and `apps/aevg_interactive` clicks, built
against the fixed tree: the spec goes from 4 passing / 6 failing (a
segfault in the click handler) to 6 passing, and both shapes recolour.

## What aether-ui can drop

The `kept = h; e.cb = kept` aliases in `vg/grammar/element.ae` and
`vg/grammar/events.ae` (and the comments pointing at the ask) once it pins
a release with this. They are harmless with the fix, so there is no hurry.
