# Closures and Environment Lifetimes

This document covers how closure environments are allocated, captured,
and released: the eight capture and lifetime bugs that shaped the mechanism
(fixed, and documented below as history), where a closure's environment is
held and released, place by place, and the patterns that still need a
workaround. The ownership rules themselves are stated once, in
[memory-management.md](memory-management.md#values-the-program-owns); this
document shows how each closure shape follows them.

Five patterns are tracked: three around dynamic `call()` dispatch
(L1, L2, L3), one correctness hazard in closures inside actor handlers
(L4), and one on closure-variable reassignment (L5). L4 is rejected at
compile time with a clear error; the rest are described in "Closure
patterns and workarounds" below.

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

Under the ownership model a `return` hands the caller a reference (R3): a
closure local at its last use moves its own reference out, and a closure
that captured another holds a reference of its own to it (#2494). The
return-site suppression, and the escape claim that replaced it, are gone.

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
nested block" Python-style rule that the calculator example
(`examples/07-applications/04-calculator.ae`) relies on.

## Code layout

Nearly all changes are in the codegen layer. The typechecker is unchanged.

| Concern | File |
|---------|------|
| Capture discovery, type resolution, return-type inference, `call()` node_type propagation | `compiler/codegen/codegen_expr.c` |
| Mutated-capture write path (routes through `_env->`) | `compiler/codegen/codegen_stmt.c` |
| Closure takes and releases at stores, scope exits and calls | `codegen_stmt.c`, `codegen_expr.c` |
| Small additions to `CodeGenerator` state | `compiler/codegen/codegen.h` |
| New helpers on the public header | `compiler/codegen/codegen_internal.h` |

## Environment reclamation

A capturing closure's environment is a heap allocation, reference counted
with atomic counts (#2494): every holder (the local it is bound to, each
container slot, struct field, message field, module `var` or state slot
that keeps it, each env that captured it) holds one reference and gives it
back when it lets go, and the last one tears the env down through its
generated destructor, which releases what the env holds. A teardown that
releases another env (a closure holding the closure it replaced) is queued
rather than nested, so a chain of any length is torn down in constant
stack. The rules are R1 to R5 of
[memory-management.md](memory-management.md#values-the-program-owns);
place by place:

- **Bound to a local.** `g = || { ... }` owns the reference the literal
  made, and the local releases it when its scope ends, on every exit.
  Rebinding the local releases the env it replaces. A local bound to
  another closure local, a parameter or a field (`last = get`, `x = h.cb`)
  takes a reference of its own, so the two release independently, and a
  local at its last use moves its reference out (a return, a store that
  takes it). A field's name is not a use of a local spelled the same
  (`Hooks { set: f }`, `h.set` beside a local `set`, #2669).

- **Passed to a call.** A call borrows its arguments (R3): a capturing
  closure created inline and passed to a function, a closure, a typed
  function pointer or an extern is released right after the call, and a
  callee that keeps it (stores, returns or captures it) takes a reference of
  its own. Nothing is read from the callee to decide it. An extern that
  keeps a closure past the call declares the parameter `@retain` (R4), and
  the call hands it a reference: `observe`, the worker pool, the http and
  net server route handlers and the QuickJS binding do. A closure passed to
  a `ptr` parameter is boxed: on the caller's stack for a parameter that
  only borrows, on the heap, with its own reference, for one declared
  `@keep` or `@retain`, and for one declared `@into`, whose result holds it
  past the call (#2990); `release_closure` gives that reference back. A
  call through a function value (`f = lend`, then `f(|x| { ... })`) boxes
  it the same way, by what the value's type says the parameter does
  (`fn(@keep ptr)`, #3035).

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

- **Captured by another closure.** Every env that captures the closure
  takes a reference, given back by its destructor. So a closure captured by
  another can be released by its own scope while the capturer, wherever it
  went, keeps it.

- **Returned to a caller.** A `return` hands the caller one reference: a
  local at its last use moves its own out, and anything else (a parameter,
  a field, a closure the function still holds) is retained at the
  `return`. So a function that returns its parameter (`keep(f) -> fn {
  return f }`) gives the caller a reference of its own, and `g2 = keep(g)`
  leaves `g` and `g2` each owning one. A closure result thrown away, or
  passed straight to a call (`x = take(make_counter())`), is released
  after that call.

- **Capturing a struct.** A struct that owns strings or closures is
  captured as a copy that retains every part it owns (`_aether_copy_<Name>`),
  dropped with the env (#2504), so the declaring scope's drop cannot release
  what a returned or stored closure reads. A struct the closure writes is a
  shared cell instead.

- **In a receive arm.** A handler (and a timeout arm) is a scope (#2498):
  its defers, cell releases, env releases and struct drops run when the
  handler ends. What the handler stores into actor state takes a reference
  of its own. A state field summed in a loop is written as the field
  (#2505).

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
function, the cell lives exactly as long as something can still reach it.
No escape analysis decides, so there is nothing to get wrong in either
direction. (Before #2019 the cell was plain-freed at scope exit only when
an escape walk could prove no env outlived the scope, and every shape the
walk could not see through, such as a callback passed inside a tuple
destructure or a closure owned and freed by an extern, leaked one cell per
call.)

The count is atomic, as the env count is: an env that holds the cell can
be released on a worker thread while the declaring scope releases its own
reference on another. A string cell owns a counted string: a store takes
the value as any place does (a fresh value moved, a counted one retained,
C's characters copied), and releases the one it replaces; the cell's last
release releases the value it still holds. A literal is stored as it is
and never freed.

An optional a closure writes (`let a: int? = 3` then `|| { a = 5 }`) is a
cell like any other (#2795): its first binding makes the cell, holding
`none` until it stores the value, and every store takes the value as the
optional, a bare value wrapped, whether by `a = v` or a tuple destructure
slot. A `string?` cell owns its string as a string cell does: a store takes
the new string and releases the one it held, and a `string?` parameter's
cell takes a reference of its own on entry. What a local takes out of a
`string?` cell (`v = s ?? ""`, `v = s!`, a `some(v)` binding) holds a
reference of its own, as what it takes out of a `string` cell does, so it
outlives the cell's next store. A string stored into a `ptr` cell (`|| { x = s }`) reaches C as its
characters, as a plain string stored by the cell's own scope does: the
cell keeps a string of its own beside the pointer, given back at the next
string store or the cell's last release, so the characters live as long as
the variable, not as long as the closure that stored them (#2797). A store
of any other pointer leaves that string where it is until then.

A module-level `var` is never a cell. A closure that reads or writes one
reaches the global itself, so a write in the closure is seen by every
function, and a write in the enclosing function is seen by the closure
(#2805). A declaration in a function (`int n = 5`, `let n = 5`) writes
the global as `n = 5` does, so only a parameter of the same name shadows
it, and that parameter is captured as any other.

A `fn` variable a closure writes is a closure cell, and it holds a
reference of its own to the closure in it (#2755), as a field, a global or
actor state does (#2525). Its first value and every store are taken: a
fresh closure is moved in, any other (a parameter, another variable, a
field) retained, so a `fn` parameter's cell never holds its caller's
reference and the caller releases the closure it passed as after any call.
A store gives back the closure it replaces, after the new one is taken,
and the cell's last holder gives back the one it still holds. A call
through the cell, or one that passes the cell's closure to an Aether
function or closure, holds a reference for the call, so a closure that
rebinds its own cell while it runs (`state = || { state = next }`) is not
freed under itself; whatever keeps or returns the cell's closure takes a
reference of its own (#2788). A closure in the cell that captures the cell
itself is a reference cycle, and is not reclaimed.

A fixed-size array the closure writes (`arr[i] = v`, `arr[i]++`, a whole
`arr = [...]`) is a cell too (#2474): a pointer to the whole array,
`int (*arr)[3]`, so `(*arr)`, which every use of a promoted name reads,
is the array itself, and indexing, `.len`, passing it as a slice and
nested closures read it as they read the array. A string array's cell
owns its elements, as a string cell owns its one: a store takes the new
element and releases the one it replaces, and the last release releases
every element. An actor's state array is not a capture: a closure in a
handler that writes one is refused, as for any state field. A string array
a closure only reads is copied into its environment, which takes a
reference to each element (#2618), so the closure can outlive the array.

A cell first assigned inside a loop body or an if-arm is hoisted ahead of
that loop or branch like any other such variable (#2024): declared as the
cell, zero-filled, at the hoisting scope, and released when that scope
ends, so it is one cell across the iterations, exactly as the hoisted
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
A tuple destructure declares each name it binds as `name = ...` does, on
either side: `a, b = three()` before the closure is a binding of `a` and `b`
the closure's `a, b = two()` writes, and so is `a = 1; b = 2` (#2762). Before,
only a single `name = ...` counted as a binding there, so a closure that
destructured into names a destructure had bound got locals of its own.
Each slot that writes an outer local is checked as `a = 7` there is: the
local keeps its type, so a slot of another kind is a compile error at the
slot (see "A local has one type" in the language reference).
A `while` body's variable is one for the function, so its cell is one across
the passes, as above; a `for` body's or a branch's, used only inside it, is
the body's own, so its cell is made at the declaration and released at the
end of each pass, and a closure kept past the pass, in a list or a struct,
holds the cell through its env.

The variable a C-style `for` loop's init binds is the loop's, so a closure
that writes it gets one cell for the whole loop (#2737): made before the loop,
read and written through by the condition, the increment and the body, and
released after the loop, as for `i = 0` before a `while`. A closure made in
any pass shares it, one kept past the loop holds it through its env, and a
`break`, `continue` or `defer` in the body behaves as in the `while`. Before,
the `for` header declared the variable as a plain value while every use read
it through the cell, and the program did not compile.

A builder block (`window(...) { ... }`, `vstack(4) { ... }`) is a scope
like any other here. Its body is emitted inside C braces and opens a
matching scope, so a cell or a string local declared in the block is
released at the end of the block rather than at the end of the enclosing
function.

Two shapes used to leak the env (both bit `std.spec`, #1577): a `fn`
parameter passed on to another function (`it(cb) { it_impl(cb) }`), and
`list.add(l, box_closure(f))`, which stored a raw pointer the list did not
know it owned. Both are ordinary shapes now: a parameter is borrowed, so
passing it on keeps nothing, and the explicit box is the same store as
`list.add(l, f)`, which gives the list a reference of its own.

## A closure's `ptr` parameters, and what its type says

A closure's `ptr` parameter borrows, as a named function's does (E0902):
the closure may not keep it past its own call. One that keeps it declares
it `@keep` (`|el: @keep ptr, t: float|`), and one whose result holds it
declares it `@into`, with the same rules as a named function's
([String ownership](language-reference.md#string-ownership)). The mode is
part of the closure's type, `fn(@keep ptr, float)`, and so of every type a
place that holds closures can be declared with (#2980):

- **A call through the value is checked by its type.** `tick(el, t)` with
  `tick: fn(@keep ptr, float)` hands `el` to a keeping parameter, so a
  borrowed `el` is E0902, a string goes only through `string.keep(s)`, and
  an `@into` result holds its argument, as for a direct call.
- **A literal declares the mode, or takes it.** A closure given for a
  closure type that names a mode takes, for each parameter it leaves
  untyped (`callback |el, t| { ... }`), the parameter's type and mode. A
  parameter it types itself keeps what it says: `|el: ptr, t: float|`
  borrows, which fits a keeping type.
- **A named function is a value of its type.** `fill(el: @keep ptr, t:
  float)` passed to `tick: fn(@keep ptr, float)`, or bound to a local, is
  a value of `fn(@keep ptr, float)`.
- **Never the unsound direction.** Borrowed, `@into`, `@keep`: a function
  goes where its modes or stronger ones are expected, never where a weaker
  one is, since a call through the weaker type would hand it a pointer the
  caller still owns. So a keeping closure does not go into a bare `fn`,
  into a weaker closure type, into a `ptr` (`box_closure`, a list: the box
  opens to a bare `fn`), or into a typed C function pointer, whose
  parameters borrow. Each is E0902, naming the parameter and both types.

This is what lets aether-ui's `vg.animate` hand its tick the element the
animation manager holds: `animate(node, ..., tick: fn(@keep ptr, float))`,
and a tick that passes the element to `vg.element_set_fill`, whose `el` is
`@keep` because a behaviour tween registers it with the manager.

## Closure patterns and workarounds

L1 to L3 are one limitation seen from three sides: a closure that has
crossed an erasing boundary has no signature, so the compiler cannot know
what a call to it returns. L4 is a compile-time rejection of what used to
give silent wrong answers. L5, a leak on reassignment, is fixed and kept
here with its tests.

### L1 to L3. Calling a closure whose signature has been erased

A closure loses its signature at a bare `fn` parameter or return, at
`box_closure`, and in a list:

```aether,fragment
handlers = list.new()
list.add(handlers, box_closure(| _ | { return "hello" }))
h = unbox_closure(list.get_raw(handlers, 0))       // L1: from a list

op = if user_wants_add { add_fn } else { mul_fn }  // L2: chosen at run time (#2055)

x = setup()                   // L3: `-> fn` erases
y = wrap(x)                   // `(f: fn) -> fn` erases again
```

What `call(h)` returns is then unknown to the compiler. **The binding says
what it is** (#2054): declare the type of the variable the call initialises
and the call is emitted for that type (a string, a pointer, a float, an
int), through either form of the call:

```aether,fragment
string greeting = call(h)
string again = h()
let p: ptr = call(y, 5)
float f = y(9)
```

Anywhere else (an untyped binding, an argument, an operand) the call is
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

An `if`-expression choosing between two named functions (L2) builds and
runs; #2055 fixed the C it used to emit.

The arguments have the same problem the other way: the closure's parameter
types are gone too. `none` and `[]` are built as a value of the slot they
are given to, and through an erased call there is no slot type to build
them as, so `call(f, none)` and `f([])` on a bare `fn` are refused at the
argument. Bind the value to a local of the parameter's type and pass that:

```aether,fragment
let o: string? = none
r = call(f, o)
```

A bare value, a variant or a number given through an erased call to a
parameter that takes it only coerced (an optional's, a sum's, a number of
a wider type) is coerced as a direct call coerces it (#2847). The function
a closure value points at still has the parameter types: the call compares
the closure's function with each closure literal and function value of the
program whose parameters take these arguments only coerced by the
assignment rules, never narrowed, and calls the one it is through its own
signature, the arguments wrapped or widened, so `call(f, 5)` into
`|o: int?|` sees 5, an int into `|x: float|` is 5.0, and a Circle into
`|o: Shape?|` is the Shape. A closure that would take an argument only by
narrowing it (a float into `|a: int|`) is not a target of the call. A
closure the program did not compile (one C hands back) is called with the
arguments as they are.

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

**Status:** the compiler walks every closure body inside every actor
receive arm and, for each write to a state field, reports an error at
the offending line with a suggestion to use the arm-local workaround.
Regression pinned by `tests/integration/closure_actor_state_reject/`.

**Workaround:** copy state into an arm-local first, mutate that, then
write back. See `tests/syntax/README_closure_actor_state_limitation.md`
for the full pattern.

A closure that only *reads* a state field compiles: it captures the
field's value when the closure is made, read through `self` as the arm
reads it, as it captures a local (#2890). A closure kept past the handler
holds that value, not the field's later ones. The copy must hold what it
reads, because the next store into the state releases what the field
held, so the copy takes a reference of its own to everything it holds:
scalars, enums, bitsets and handles (`ptr`, actor references) are values;
a string or a closure is retained; a struct, a tuple, an optional, a sum
and a fixed-size array take a reference to each part they own (#2921,
#3005); a slice takes one to its buffer, which is counted, so a store into
the state after the closure was made leaves the closure's copy whole
(#3568). Anything else (a fixed-size array of slices) is refused at
compile time, naming the field: read what the closure needs into arm
locals first and use those.

**Proper fix:** thread `self` through the closure's env so state
writes compile to `self->field = ...`. Medium-sized codegen change;
until it lands, the compile-time rejection prevents silent wrong
answers.

### L5. Closure-var reassignment leaked the previous env

```aether,fragment
op = | x: int | { return x + 1 }
op = | x: int | { return x * 2 }  // a capturing old env used to leak here
```

Fixed by the ownership model: a closure local owns a reference to its
env, a rebinding releases the env it replaces, and the scope exit releases
the last one, whatever the variable was used for. Anything that still
needs the old env holds a reference of its own to it: a `box_closure()`
copy, a container or field it was stored in, a closure that captured it
(#2494). Before, an escape walk over the function decided whether a
rebinding could free (#2480, #2506), and every shape it could not see
through kept every env of the variable.

Paired tests pin this:

- `tests/syntax/test_closure_reassign_leaks_env.ae` 100-iteration
  reassignment loop exits cleanly.
- `tests/syntax/test_closure_reassign_after_box.ae` box_closure'd
  copy survives reassignment of the source variable.
- `tests/integration/closure_local_env_free` counts env and cell
  allocations against frees for owned and escaping shapes.

## Why the UI calculator works

`examples/07-applications/04-calculator.ae` uses ref cells for all mutable state and
builds handlers as anonymous inline closures passed to `box_closure(...)`.
Ref cells sidestep bug 2 (the closure captures a pointer, read-only from
its perspective), and anonymous box-wrapped closures sidestep bug 3 (no
named variable, no defer-free insertion). It worked before these fixes
and still works.
