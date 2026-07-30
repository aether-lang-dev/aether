# `Isolated[T]`: move-only actor message payloads

Aether's actor model type-checks message shapes, but declared types alone do
not stop a sender from keeping a reference to a heap-bearing value it just
handed off. For ref-counted strings that is papered over by reference counting;
for pointer-bearing payloads (a `*StringSeq`, a struct with a file handle, a
future capability token) it is a real aliasing hazard.

`Isolated[T]` is the small, zero-cost answer, borrowed from Nim's `Isolated[T]`
(itself descended from Pony's reference capabilities): a wrapper type that is
**move-only** at the language level. You build one with `isolate(x)`, you unwrap
it once with `consume(iso)`, and the compiler rejects any second use. It is a
compile-time discipline only: it lowers to the wrapped type's C type with no
runtime structure and no overhead.

```aether
process(iso: Isolated[Payload]) -> int {
    let p = consume(iso)     // unwrap once
    return p.n
}

main() {
    let p   = Payload { n: 21, tag: "x" }
    let iso = isolate(p)     // wrap: `p` (a struct) is now moved-from
    let r   = process(iso)   // ownership transfers into the callee
    // consume(iso) here would be a compile error: iso was already moved
}
```

## The four design questions

**1. Built-in type, or a stdlib type with a compiler pragma?**
Built-in. `Isolated[T]` is a compiler-known type (`TYPE_ISOLATED`) whose single
type parameter is the wrapped `T`. It is nominal (never implicitly convertible
to or from a bare `T`) and lowers to `T`'s C type, exactly like a `distinct`
type. A stdlib struct would have forced a runtime box and lost the
zero-cost property; a built-in keeps the whole feature in the type checker.

**2. How does `isolate(...)` establish that nothing else aliases the value?**
v1 enforces *linearity of the wrapper and of a move-worthy source binding*, not
a proof that the transitive heap graph is unaliased:

- The `Isolated[T]` value is move-only. Every use of it is a consuming move, so
  a second use, or any use after `send` / `consume`, is a compile error.
- `isolate(x)` additionally consumes its source when `x` is a local of a
  move-worthy type (a heap or reference or aggregate type: string, `ptr`,
  struct, array, message, actor ref, tuple, sum, optional, function). After
  `isolate(x)`, referencing `x` by name is a use-after-move. Copyable scalars
  (`int`, `float`, `bool`, `byte`, duration) are not consumed by `isolate` (a
  loop counter passed to `isolate(i)` stays usable), because there is no
  ownership to relinquish.

This gives the guarantee the motivation asks for at the binding level: once a
value is isolated and handed off, the original binding cannot touch it. It does
*not* yet prove that no other pre-existing alias into the value's heap graph
exists; a deeper escape/aliasing check is a documented follow-up (see below).

**3. Does `receive` auto-unwrap, or hand the handler the wrapper?**
Explicit. The holder of an `Isolated[T]` calls `consume(iso)` to obtain the
`T`. Keeping the unwrap explicit keeps the single move point visible in the
source and mirrors the existing `release(x)` builtin style, rather than hiding a
move inside pattern binding.

**4. Interaction with structural sharing (e.g. `*StringSeq`'s shared tails)?**
Isolation in v1 is shallow: the linearity is enforced on the wrapper and its
immediate source binding, not on the transitive graph. A deep-share value can
be wrapped, and the wrapper's single-use discipline still holds, but v1 does not
verify that the value's internals are unshared. The honest rule is "isolation is
a binding-level move discipline; structural non-aliasing of the payload is the
programmer's responsibility until the deep check lands."

## The move checker

Enforcement is a forward pass over each function body (`iso_check` in
`compiler/analysis/typechecker.c`). It tracks, per control-flow path, the set of
names already moved:

- **Sequences**: a move carries forward to later statements.
- **`if` / `else`**: each branch is analyzed on a copy of the incoming set; the
  join is their union, skipping a branch that diverges (ends in
  `return` / `break` / `continue`). So a value consumed on *both* branches is
  moved afterward, while one consumed on a branch that returns does not taint
  the fall-through.
- **Loops**: the body is analyzed twice, so consuming a loop-external Isolated
  inside the body (which would repeat across iterations) is caught, while a
  value created fresh and consumed each iteration is fine.
- **Rebinding**: assigning or re-declaring a name revives it (a fresh binding),
  so `it = isolate(...)` after a previous `consume(it)` is allowed.

The diagnostic is `use of moved value '<name>'` (error code E0200).

## What lowers and runs today, and what is a follow-up

Working and covered by tests
(`tests/regression/test_isolated_basic.ae`,
`tests/integration/isolated_move_reject/`):

- `Isolated[T]` for scalar, string, and struct payloads.
- `isolate()` / `consume()` and ownership transfer into a function.
- The full move checker (use-after-consume, use-after-`send`, heap-source
  reuse, loop-external consume all rejected; single-use, both-branch consume,
  fresh-per-iteration, and scalar-source reuse all accepted).
- Zero-cost lowering: `Isolated[T]` is `T` in the emitted C; `isolate` and
  `consume` are the identity.

Deliberate follow-ups (each a natural next implementation phase):

- **Actor mailbox payloads.** Isolating a `message` *constructor*
  (`isolate(Task{...})`) and flowing it through the actor mailbox with
  auto-unwrap in `receive` is not wired yet: messages have a distinct
  construction and enqueue path from plain values. The move checker already
  enforces linearity at a `send(w, iso)` call site; only the message-payload
  codegen remains.
- **Deep aliasing / escape proof** for question 2 above, so `isolate(x)` can
  reject wrapping a value that another live binding still references.
- **`Isolated[Capability]`** as the type-level shape of a single-use, non
  -shareable capability token, once capability tokens are a first-class type.
