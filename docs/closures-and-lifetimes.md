# Closures and Environment Lifetimes

This document covers how closure environments are allocated, captured,
and freed, and the patterns that currently need workarounds. Earlier
rounds of the closure/DSL feature shipped with four capture-handling and
env-lifetime issues plus a chained typechecker hole that surfaced once
those four were resolved. All five landed together in a single PR
because testing any one in isolation was blocked by the others; they
are fixed on main and documented below as the underlying mechanism.

Three more closure-related bugs surfaced during the aether_ui toolkit
work: emission-ordering for cross-referenced closures, nested-lambda
return-type bubble-up, and captures across trailing blocks. All three
are fixed on main.

Five patterns are tracked, three around dynamic `call()` dispatch
(L1, L2, L3), one correctness hazard in closures inside actor handlers
(L4), and one memory-handling contract on closure-var reassignment
(L5). L4 is now rejected at compile time with a clear error; the rest
are documented with workarounds in the "Closure patterns and
workarounds" section below.

Regression tests live at `tests/syntax/test_closure_*.ae` and
`tests/integration/closure_*/`.

## The bugs

### 1. Captured `ptr` parameters typed `int`

A closure capturing a `ptr` parameter of its enclosing function got
`int <name>` in the env struct, pointer truncated on store, segfault on
deref. Capture-type resolution in `compiler/codegen/codegen_expr.c` walked
only top-level `AST_VARIABLE_DECLARATION` nodes across all functions and
returned the first match by name, so it never saw function parameters and
silently returned the type of any unrelated same-named variable elsewhere
in the program.

**Fix:** thread the enclosing function name through `discover_closures`
into each `ClosureInfo.parent_func`. Capture-type lookup now searches that
function's parameters (`AST_PATTERN_VARIABLE`) and its locals in nested
blocks first, only falling back to the program-wide scan when scope is
unknown.

### 2. Mutable captures miscompiled

`count = count + 1` inside a closure body emitted a shadowing local that
wrote to uninitialised stack, silent wrong answers, even in-scope. Two
collaborating problems: `is_local_var` treated any assignment target as a
fresh local, and the closure prologue unconditionally aliased each capture
to a read-only C local.

**Fix:** keep the alias pattern for read-only captures (zero cost), but
detect captures that are assigned to in the body and route their reads
and writes through `_env->name` directly. Scope analysis distinguishes
captures from fresh body-locals while honouring Python-style `x = expr`
shadowing: if the RHS does not read `x`, treat `x` as a fresh local.

**Resulting semantics:** a read-only capture stays an aliased copy (zero
cost). A capture the closure *assigns to* is heap-promoted, so the enclosing
binding and the closure env share one heap cell and writes are visible in both
directions (the Ruby/Groovy model, verified by
`tests/syntax/test_closure_mutable_capture_probe.ae`). A simple named local
therefore needs no ref cell to share mutable state; ref cells remain useful for
state that isn't a plain captured local (an explicit `ptr` argument, a struct
field, or an actor boundary). See `docs/closures-and-builder-dsl.md`.

### 3. Escaping-closure use-after-free

A closure variable `bump = || { ... }` pushed an unconditional
`free(bump.env)` onto the defer stack; at return the compiler emitted
every defer before returning, including the env-free of the closure being
returned. The caller received a closure whose env was already freed.

**Fix:** at return emission, walk the return expression to collect every
closure variable that appears (including `box_closure` wrappers) and
transitively any closure vars they capture. `emit_all_defers_protected`
skips the matching env-free defers and emits a
`/* deferred (suppressed: escapes via return) */` marker in their place.
Ownership transfers to the caller, matching the documented contract for
`box_closure`.

### 4. Closure return types hardcoded to `int`/`void`

The static `_closure_fn_N` wrapper was typed `int` (or `void`) regardless
of what the closure actually returned. Closures returning a string or
pointer either tripped `-Wint-conversion` and truncated the return, or
the caller cast through int and dereferenced a truncated pointer.

**Fix:** pick the return type from the returned expression's `node_type`.
For `return call(<captured_closure>)` chains the typechecker leaves the
inner call as `TYPE_INT`, so we resolve through the captured closure's
own body.

### 5. `call()` expression `node_type` stuck at `TYPE_INT`

The global `call` builtin is symbol-typed as `TYPE_INT`, so the
typechecker set `node_type=TYPE_INT` on every `call(x)` expression
regardless of what `x`'s closure actually returned. Downstream,
`print`/`println` picked `"%d"` for calls that really returned strings,
and `s = call(w)` declared `s` as `int` so later comparisons dereferenced
a truncated pointer.

**Fix:** a post-discovery pass walks the program and, for every
`call(<known_closure_var>)` expression, rewrites `node_type` to match the
closure's actual return type. It also back-propagates into variable
declarations whose initializer is such a call. `closure_var_map` seeding
is extended to inherit a closure id through `w = f()` when `f` ends
`return <closure_var>`, so chains like `w = build_pair(); call(w)`
resolve correctly.

### 6. Closure body references a later-numbered closure

A closure's body can construct inline closure literals and pass them
as arguments to other functions. Each lambda gets its own
`_closure_fn_N` in the emitted C. When the outer closure is numbered
before its inline lambdas, its body referenced `_closure_fn_N`
symbols that hadn't been declared yet at that point in the file.
Error: `'_closure_fn_N' undeclared`.

**Fix:** `emit_closure_definitions` now runs in two passes. Pass 1
emits every env typedef and every function prototype. Pass 2 emits
bodies and constructors. A closure body can reference any
`_closure_fn_N` by name regardless of numbering.

### 7. Nested lambda's return mis-typed the enclosing closure

`has_return_value` walked an AST subtree looking for return
statements with values. A nested lambda's `return` bubbled up and
mis-typed the enclosing closure as `int`, producing a
`static int _closure_fn_N(...) { ...; }` with no return statement,
undefined behavior caught by `-Wreturn-type`.

**Fix:** `has_return_value` stops at `AST_CLOSURE` boundaries. A
nested closure's return belongs to that closure, not to any
enclosing scope.

### 8. Captures across nested trailing blocks

A variable declared inside a trailing block (e.g.
`root = grid() { c = 42; ... }`) lives in the enclosing function's
scope because trailing blocks are inlined at the call site, not
hoisted. A closure inside a sibling or nested trailing block should
be able to capture such variables. Previously, capture discovery
stopped at `AST_CLOSURE` boundaries including trailing-block
closures (value == `"trailing"`), so names declared inside one
trailing block were invisible to inner closures.

**Fix:** scope-analysis helpers treat trailing-block closures
transparently while still stopping at real closures.
`subtree_declares` recurses through trailing blocks; a new
`scope_declares_at_top_level` helper is used by
`is_top_level_decl_in_function` to walk trailing blocks but NOT
nested if/for/while blocks, preserving the "fresh body-local in
nested block" Python-style rule that `calculator-tui` relies on.

## Code layout

Nearly all changes are in the codegen layer. The typechecker is unchanged.

| Concern | File |
|---------|------|
| Capture discovery, type resolution, return-type inference, `call()` node_type propagation | `compiler/codegen/codegen_expr.c` |
| Mutated-capture write path (routes through `_env->`) | `compiler/codegen/codegen_stmt.c` |
| Bug-3 return-defer protection | `compiler/codegen/codegen.c`, `codegen_stmt.c` |
| Small additions to `CodeGenerator` state | `compiler/codegen/codegen.h` |
| New helpers on the public header | `compiler/codegen/codegen_internal.h` |

## Environment reclamation

A capturing closure's environment is a heap allocation; two common
lifetimes now reclaim it automatically (the canonical reference is
[`docs/memory-management.md`](memory-management.md) → "Closure environment
lifetime"):

- **Transient callback.** A capturing closure created inline and passed to
  a parameter that only *calls* it and neither stores nor returns it
  (`run(cb) { cb() }`) is dead once the call returns, so its env is freed
  right after the call. This is gated on a proven non-escape, invoking a
  closure parameter (`cb()`, an indirect-`call` node whose first child is
  the callee) is not an escape, whereas a stored or returned closure
  suppresses the drain so its env follows the owner.

- **Stored in a list.** A closure value stored into a list is heap-boxed
  (the `fn → ptr` coercion) and the list owns the box; `list.free` now
  reclaims the captured env as well as the box (`owned_flags == 2`).

## Mutated-capture cell lifetime

A capture the closure assigns to is heap-promoted: the enclosing binding
and the closure env share one cell (see "Capture semantics" above). The
cell is **reference-counted** (#2019). The declaring scope holds one
reference from the declaration to its exit; every env built from a
closure that captures the cell takes one when it is constructed and gives
it back in its generated destructor (`_closure_env_N_free`, the same
member-aware teardown that releases retained string captures); the last
holder to release frees the cell.

That makes the scope-exit release unconditional. Whether the closure was
drained right after a transient call, handed to `fs.walk` inside a tuple
destructure, stored by a widget, kept in a list, or returned from the
function, the cell lives exactly as long as something can still reach it
— no escape analysis decides, so there is nothing to get wrong in either
direction. (Before #2019 the cell was plain-freed at scope exit only when
an escape walk could prove no env outlived the scope, and every shape the
walk could not see through — a callback passed inside a tuple destructure,
a closure owned and freed by an extern — leaked one cell per call.)

The count is a plain integer, like the string reference count it mirrors:
a closure env is not shared between threads.

A cell first assigned inside a loop body or an if-arm is hoisted ahead of
that loop or branch like any other such variable (#2024): declared as the
cell, zero-filled, at the hoisting scope, and released when that scope
ends — so it is one cell across the iterations, exactly as the hoisted
plain variable is one variable.

A builder block (`window(...) { ... }`, `vstack(4) { ... }`) is a scope
like any other here. Its body is emitted inside C braces and now opens a
matching defer scope, so a cell, or a heap string, declared in the block
is reclaimed at the end of the block rather than at the end of the
enclosing function.

Two shapes defeat these paths and leak the env — both bit `std.spec`
(#1577):

- **Forwarding a `fn` parameter.** The transient-callback drain fires
  only when the callee *calls* its `fn` parameter. Passing it on to
  another function (`it(cb) { it_impl(cb) }`) is an escape from the
  callee's point of view, so the caller keeps the env alive forever.
  Restructure so the exported function invokes the parameter itself —
  split the shared logic into begin/end halves around the `call()` if
  needed (that is exactly how `std.spec`'s `it`/`it_within` are built).

- **Explicit `box_closure()` into a list.** The list-owns-the-env path
  is keyed off the `fn → ptr` coercion at the `list.add` call site.
  `list.add(l, box_closure(f))` hands the list a raw pointer it cannot
  know it owns; nothing reclaims box or env. Add the `fn` value
  directly (`list.add(l, f)`) and the owned coercion does the boxing
  and the reclamation.

Still a leak (the safe side of the leak-vs-UAF trade): **L5 below**,
reassigning a closure *variable* drops the previous env, because without
whole-program escape analysis the codegen can't prove the old env is
unreachable (it may be aliased through a `box_closure` copy).

## Closure patterns and workarounds

L1–L3 are one limitation seen from three sides: a closure that has crossed
an erasing boundary has no signature, so the compiler cannot know what a
call to it returns. L4 is a compile-time rejection, previously silent wrong
answers, now surfaced at compile time with a clear error. L5 is a
memory-handling contract around reassignment.

### L1–L3. Calling a closure whose signature has been erased

A closure loses its signature at a bare `fn` parameter or return, at
`box_closure`, and in a list:

```aether,fragment
handlers = list.new()
list.add(handlers, box_closure(|_| { return "hello" }))
h = unbox_closure(list.get_raw(handlers, 0))       // L1: from a list

op = if user_wants_add { add_fn } else { mul_fn }  // L2: chosen at run time (#2055)

x = setup()                   // L3: `-> fn` erases
y = wrap(x)                   // `(f: fn) -> fn` erases again
```

What `call(h)` returns is then unknown to the compiler. **The binding says
what it is** (#2054): declare the type of the variable the call initialises
and the call is emitted for that type — a string, a pointer, a float, an
int — through either form of the call:

```aether,fragment
string greeting = call(h)
string again = h()
ptr p = call(y, 5)
float f = y(9)
```

Anywhere else — an untyped binding, an argument, an operand — the call is
the `int` the language has always defaulted to, and an untyped binding is
told so at the site:

```
warning: the closure called here has no known result type, so 'r' is
assumed int; declare the binding's type (e.g. `string r = call(...)`) for
any other result
```

Before #2054 the typed form was refused as a type mismatch and the untyped
one returned the string's pointer truncated to an int, silently. A bare `fn`
is now compatible with a signed closure type in both directions, so the
erasure and its undoing are both ordinary assignments.

L2's other half — an `if`-expression choosing between two named functions —
still emits C that does not compile; that is #2055.

**Proper fix:** parameterised closure types (`fn[T]`, like Rust's
`Fn(i32) -> i32`), so the signature survives the boundary and no annotation
is needed at the call. Until then the typed binding is the contract.

### L4. Closure inside actor handler mutating actor state

```aether,fragment
actor Counter {
    state count = 0
    receive {
        Go() -> {
            inc = || { count = count + 1 }   // rejected at compile time
            call(inc)
        }
    }
}
```

Closures inside actor handlers correctly capture and mutate arm-local
variables (Route 1 + arm promotion, tested by
`tests/syntax/test_closure_in_actor_handler.ae`). But when the closure
writes a name that's an actor **state field**, the closure has no
access to `self`, so state accesses would compile to unscoped local
reads, a silent wrong answer.

**Current status (as of this branch):** codegen walks every closure
body inside every actor receive-arm and, for each write to a state
field, emits a compile-time error pointing at the offending line with
a suggestion to use the arm-local workaround. Regression pinned by
`tests/integration/closure_actor_state_reject/`.

**Workaround:** copy state into an arm-local first, mutate that, then
write back. See `tests/syntax/README_closure_actor_state_limitation.md`
for the full pattern.

**Proper fix:** thread `self` through the closure's env so state
writes compile to `self->field = ...`. Medium-sized codegen change;
until it lands, the compile-time rejection prevents silent wrong
answers.

### L5. Closure-var reassignment leaks the previous env

```aether,fragment
op = |x: int| { return x + 1 }
op = |x: int| { return x * 2 }  // old env (malloc'd) is leaked
```

When a closure variable is reassigned, the auto-defer-free fires only
on the first assignment (to avoid double-free at scope exit, since
reassignment overwrites `.env` in the variable). The previous env's
heap block is unreachable, leaked.

**Why not just free on reassignment:** the old env may still be
reachable via a `box_closure()` copy or another closure's transitive
capture. Without escape analysis we can't tell if it's safe to free,
so we lean safe (leak) over unsafe (UAF).

Paired tests pin this trade-off:

- `tests/syntax/test_closure_reassign_leaks_env.ae` 100-iteration
  reassignment loop exits cleanly.
- `tests/syntax/test_closure_reassign_after_box.ae` box_closure'd
  copy survives reassignment of the source variable.

**Proper fix:** escape analysis. Track whether a closure variable has
been captured or stored anywhere before the reassignment; if not, free
on reassignment. Larger change; deferred.

## Why the UI calculator works

`examples/calculator-tui.ae` uses ref cells for all mutable state and
builds handlers as anonymous inline closures passed to `box_closure(...)`.
Ref cells sidestep bug 2 (the closure captures a pointer, read-only from
its perspective), and anonymous box-wrapped closures sidestep bug 3 (no
named variable, no defer-free insertion). It worked before these fixes
and still works.
