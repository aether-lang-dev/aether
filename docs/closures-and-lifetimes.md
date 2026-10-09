# Closures and Environment Lifetimes

This document covers how closure environments are allocated, captured,
and freed: the eight capture and lifetime bugs that shaped the mechanism
(fixed, and documented below as the mechanism itself), the reclamation
rules shape by shape, and the patterns that still need a workaround.
The ownership rules themselves are stated once in
[`docs/memory-management.md`](memory-management.md) (string tracker,
container values, closure arguments, closure environment lifetime); this
document lists how each closure shape meets them.

Five patterns are tracked, three around dynamic `call()` dispatch
(L1, L2, L3), one correctness hazard in closures inside actor handlers
(L4), and one memory-handling contract on closure-var reassignment
(L5). L4 is rejected at compile time with a clear error; the rest
are documented in the "Closure patterns and workarounds" section below.

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
transitively any closure vars they capture, and skip their env-free
defers. Ownership transfers to the caller, matching the documented
contract for `box_closure`.

Since #2480 a returned closure local gets no env-free in the first place
(a return is an escape to the scope-exit claim), and since #2494 a closure
that captured another holds its own reference to it, so the return-site
suppression was removed: it only leaked the captured env.

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
| Env claim and release of closure locals (`claim_closure_local_env`, the env scan) | `codegen_stmt.c` |
| Small additions to `CodeGenerator` state | `compiler/codegen/codegen.h` |
| New helpers on the public header | `compiler/codegen/codegen_internal.h` |

## Environment reclamation

A capturing closure's environment is a heap allocation, reference counted
(#2494): every holder (the local it is bound to, each container slot,
struct field, message field, global or state slot that keeps it, each env
that captured it) holds one reference and gives it back when it lets go,
and the last one tears the env down. The rules are stated in
[`docs/memory-management.md`](memory-management.md) → "Closure environment
lifetime"; shape by shape:

- **Transient callback.** A capturing closure created inline and passed to
  a parameter that only *calls* it and neither stores nor returns it
  (`run(cb) { cb() }`) is dead once the call returns, so its env is freed
  right after the call. This is gated on a proven non-escape, invoking a
  closure parameter (`cb()`, an indirect-`call` node whose first child is
  the callee) is not an escape, nor is passing it on to a function whose
  body only calls it (`it(cb) { it_impl(cb) }`), whereas a stored or
  returned closure suppresses the drain so its env follows the owner. An
  extern has no body to read, so a closure passed to one is kept unless
  the extern declares the parameter `@noescape` (used only during the
  call, #2523): the std seq combinators, `fs.walk` and `string_list_sort`
  do, so their callbacks are released after the call like any transient
  callback.

- **Stored in a list or a map.** A closure value stored into a list or a
  map is heap-boxed (the `fn -> ptr` coercion, or an explicit
  `box_closure(f)` at the store, which is the same store) and the
  container owns the box and a reference of its own to the env (#2518),
  both released when the element leaves (`list.remove`, `list.clear`, a
  closure set over the slot, `map.remove`, a put over the key, `free`).
  A literal or a handed-over call result stored this way is released
  right after the store; a local stored this way keeps its own reference
  and releases it at scope end.

- **Kept in a struct field, a message field, a global or an actor's
  state (#2525).** Each such holder has a reference of its own to the env,
  the way a struct owns its string fields (#2497): a store takes one (a
  fresh closure's is adopted, a view of one held elsewhere, a local, a
  parameter, another field, is retained), overwriting the slot and
  destroying the holder release it, and a copy retains. So `h = Holder {
  cb: build("a") }; h.cb = build("b")` releases both environments, `b = a`
  and a struct passed or returned by value share the closure through two
  references, a fixed-size array field of such structs releases every
  element, a message's closure field is released with the message once
  the handler is done (a handler that keeps it in state retained its
  own), and a global or state slot gives back the env it held when
  rebound. The closure local stored into a field still releases its own
  reference at scope end. A local bound to a field read (`x = h.cb`) and
  a closure a named function returns from a field hold references of
  their own, so the struct may go first. A `fn[N]` field holds a reference
  per element, a local array of such structs owns its elements, an
  actor's closure state is released with the actor (its `destroy_state`
  hook, run once when the scheduler ends it), a reply's closure fields are
  the asker's (the one it reads out) or released with the reply, and a
  closure literal handed to a function that stores it in a field is
  released by the caller after the call (#2528).

- **Bound to a local.** `g = || { ... }` frees its env when the local's
  scope ends, through `_closure_env_N_free`, provided every use of `g`
  leaves no copy behind: calling it, passing it to a user function whose
  parameter is only called or passed on the same way, or capturing it in
  a closure, or storing it where the holder takes a reference of its own
  (a list, a map, a struct field, a message field, a global, actor state,
  #2480). Rebinding the local to a new closure frees the env it replaces.
  A return, an alias, an argument to an extern parameter not marked
  `@noescape` or to a function that keeps it, or a binding to anything
  but a fresh closure leaves the env to the value's holder.

- **Captured by another closure.** An env is reference-counted (#2494):
  the value's owner holds one reference and every env that captured the
  closure takes one, given back by its destructor (which is what every
  owner calls to free an env). So a closure captured by another can be
  freed by its own scope while the capturer, wherever it went, keeps it.

- **Returned to a caller.** A function whose every `return` hands back a
  closure nothing else holds (a closure literal, a local whose only
  escape is the return, or another such function's result) gives its
  reference to the caller, and a local bound to its result is freed like
  a local bound to a literal (#2494). Passed to a call whose parameter
  keeps nothing, anywhere in an expression (`x = take(make_counter())`),
  it is freed after that call (#2506, #2507); thrown away, it is freed at
  once.

- **Handed on, then rebound.** A local whose value is handed on (stored,
  returned, aliased) stops owning it right before the statement that does
  it; `_envown_<name>` records it, and the closures the local is bound to
  afterwards are still freed (#2506). The statement may be a condition, a
  loop whose body does not rebind the local, a statement with a trailing
  block, or a `defer`, whose deferred statement is the point (#2507).

- **Capturing a struct.** A struct that owns strings is captured as a copy
  with strings of its own (`<Name>_dup`), destroyed with the env (#2504),
  so the declaring scope's destroy cannot free what a returned or stored
  closure reads. A struct the closure writes is a shared cell instead.

- **In a receive arm.** A handler (and a timeout arm) is a defer scope
  (#2498): its defers, cell releases, env frees and struct destroys run
  when the handler ends. A struct local stored into actor state, or
  its strings (a closure that captured it has its own copy, #2504). A
  state field summed in a loop is written as the field (#2505).

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

The count is atomic, as the env count is: an env that holds the cell can
be released on a worker thread while the declaring scope releases its own
reference on another. A string cell owns a refcounted string: a store
takes the value as an owning slot does (a fresh value adopted, a local
moved or copied, a view copied), and a plain owned buffer (an `@heap`
extern's strdup) becomes a refcounted copy on the way in, so the cell's
last release frees every value it was ever given; a literal is stored as
it is and never freed.

A fixed-size array the closure writes (`arr[i] = v`, `arr[i]++`, a whole
`arr = [...]`) is a cell too (#2474): a pointer to the whole array,
`int (*arr)[3]`, so `(*arr)`, which every use of a promoted name reads,
is the array itself, and indexing, `.len`, passing it as a slice and
nested closures read it as they read the array. A string array's cell
owns its elements, as a string cell owns its one: a store frees the
element it replaces and takes a buffer of its own, and the last release
frees every element. An actor's state array is not a capture: a closure
in a handler that writes one is refused, as for any state field.

A cell first assigned inside a loop body or an if-arm is hoisted ahead of
that loop or branch like any other such variable (#2024): declared as the
cell, zero-filled, at the hoisting scope, and released when that scope
ends — so it is one cell across the iterations, exactly as the hoisted
plain variable is one variable.

A closure that assigns a name writes the binding of it that is visible from
the closure and comes before it (#2659): a parameter, a declaration among the
statements of the enclosing function or closure, or one in a block on the way
down to the closure, the body of the loop, branch or `match` arm it sits in,
or a trailing block. A binding in a sibling block (another loop or branch
body, a sibling trailing block) is another variable, and the closure's
assignment is then a fresh local of its own (#2189). Before, only the
enclosing scope's top-level statements and trailing blocks counted, so
`c = 0; f = || { c = 5 }` in a loop body gave the closure a fresh `c` and the
loop's `c` never changed (a closure that also read `c` captured it already).
A `while` body's variable is one for the function, so its cell is one across
the passes, as above; a `for` body's or a branch's, used only inside it, is
the body's own, so its cell is made at the declaration and released at the
end of each pass, and a closure kept past the pass, in a list or a struct,
holds the cell through its env.

A builder block (`window(...) { ... }`, `vstack(4) { ... }`) is a scope
like any other here. Its body is emitted inside C braces and now opens a
matching defer scope, so a cell, or a heap string, declared in the block
is reclaimed at the end of the block rather than at the end of the
enclosing function.

Two shapes used to defeat these paths and leak the env (both bit
`std.spec`, #1577): a `fn` parameter passed on to another function
(`it(cb) { it_impl(cb) }`), which the callee walk counted as kept by its
kind before reading the callee's body, and `list.add(l, box_closure(f))`,
which stored a raw pointer the list did not know it owned. Both are the
ordinary shapes now: the callee's body decides, and the explicit box is
the store `list.add(l, f)` is.

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
let p: ptr = call(y, 5)
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

A closure that only passes such a call through, `|| { return call(f, x) }`,
has no result type of its own either. A typed use of it gives it one (#2484):
`let r: ptr = call(g)` or `return call(g)` from a `-> ptr` function types
`g`'s return, so the pointer is not cut to an int on the way out. A closure
that never meets such a use (one handed to an extern, or returned through
`-> fn`) is typed int and warned about at its `return`; bind the inner
result with its type and return that:

```aether,fragment
task = || { let r: ptr = call(f, item); return r }
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

### L5. Closure-var reassignment leaked the previous env

```aether,fragment
op = |x: int| { return x + 1 }
op = |x: int| { return x * 2 }  // a capturing old env used to leak here
```

Fixed for locals whose value never leaves the scope (#2480): an escape
walk over the function proves every use of the variable is a call or an
argument to a parameter that keeps nothing, and then each rebinding frees
the env it replaces and scope exit frees the last one.

The old env may still be reachable via a `box_closure()` copy; the
variable stops owning it right before that statement (#2506), so the
replaced env is left to the copy while later bindings are freed. A
hand-off inside a nested closure body (another C function, which cannot
reach the flag) or around a rebinding of the variable in the same
statement still keeps every env of the variable. A closure that captured the old value holds
its own reference to it (#2494), so it is no reason to keep the env.

Paired tests pin this:

- `tests/syntax/test_closure_reassign_leaks_env.ae` 100-iteration
  reassignment loop exits cleanly.
- `tests/syntax/test_closure_reassign_after_box.ae` box_closure'd
  copy survives reassignment of the source variable.
- `tests/integration/closure_local_env_free` counts env and cell
  allocations against frees for owned and escaping shapes.

## Why the UI calculator works

`examples/calculator-tui.ae` uses ref cells for all mutable state and
builds handlers as anonymous inline closures passed to `box_closure(...)`.
Ref cells sidestep bug 2 (the closure captures a pointer, read-only from
its perspective), and anonymous box-wrapped closures sidestep bug 3 (no
named variable, no defer-free insertion). It worked before these fixes
and still works.
