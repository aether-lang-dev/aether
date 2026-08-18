# Closures and Builder-Style DSL in Aether

Aether supports closures, trailing blocks, and a builder-style DSL pattern inspired by
Smalltalk's blocks, Ruby's blocks/procs, and Groovy's closures. This document covers the
syntax, semantics, and the builder context mechanism that enables pseudo-declarative DSLs.

The compact name for this whole language feature, closures + trailing blocks executing
in the caller's context to give pseudo-declarative scripts, is **"DSL with scope"**,
the term Yukihiro "Matz" Matsumoto (Ruby's creator) said he's always used for it. See
[Paul Hammant's "That Ruby and Groovy Language Feature" (2024-02-14)][matz-dsl-scope]
for the genealogy from Smalltalk through Ruby, Groovy, Kotlin, SwiftUI, and the
side-by-side calculator-app comparison across each.

(Another lens on the same idea, less catchy but mechanically descriptive: *"closure
with context"*, the trailing block is a closure that runs with an implicit
context object available to its body, which is what `_ctx: ptr` injection and the
builder context stack provide in Aether.)

[matz-dsl-scope]: https://paulhammant.com/2024/02/14/that-ruby-and-groovy-language-feature

## Quick reckoner (LLM cheat-sheet)

Two **independent** axes describe every trailing-block usage:

**Axis 1, Trailing-block form (what the *caller* writes after `)`):**

| Form | Syntax | Runs | Captures |
|------|--------|------|----------|
| **Immediate** | `func() { body }` | Inline, as DSL structure | Sees enclosing scope directly (it's just inline code) |
| **Closure (parameterised)** | `func() \|x\| { body }` or `\|x\| -> expr` | Later, when invoked via `call(...)` | Explicit parameters; captures enclosing locals by value |
| **Callback** | `func() callback { body }` (optionally `callback \|x\|` or `callback \|x\| -> expr`) | Later, when invoked via `call(...)` | Implicit capture of enclosing-scope locals (closure with no required params) |

**Axis 2, Trailing-block-accepting function (what the *definition* declares):**

| Flavour | Definition | Order | Block provides | Function provides |
|---------|------------|-------|----------------|--------------------|
| **Regular** | plain `func(...)` (often `_ctx: ptr` for auto-injection) | Function first, then block | Decoration / children | The container/value to fill |
| **Builder** | `builder func(...)` (optionally `with <factory>` for the config object) | **Block first**, then function | Configuration (a config object filled via setter calls) | The action that uses the filled config |

**The axes multiply.** Any of the three forms on the call side can target either flavour on the
definition side, giving six legal combinations. Builder is not a fourth trailing-block form.
It's a function-side contract that any of the three trailing-block forms can attach to.

**Related supporting machinery (covered later):**

- **Builder context stack** (`_ctx: ptr` auto-injection, `builder_context()`, `_aether_ctx_push/pop`)
  wires parents to children automatically inside Immediate trailing blocks.
- **Block-receiver scoping**, `receiver.method(...) { body }` falls back through `<receiver>_<name>` for bare-name calls inside `body`.
- **Ref cells** (`ref(v)` / `ref_get` / `ref_set`), shared mutable state for closures (which otherwise capture by value).
- **Boxed closures** (`box_closure` / `unbox_closure`), heap-allocate a closure so it can be stored in `std.list` / struct fields.
- **`fn` type**, the parameter type for "this function takes a closure"; invoked with `call(fn_var, args...)`.

**Same-line rule** (line 100+): a trailing block is attached to a call only when `{`
appears on the same line as the call's closing `)`. A `{` on the next line is an
independent block. This is what stops `result = build()\n{ ... }` from greedily
consuming the bare block as `build`'s closure.

**Separate axis, fluent method chaining (UFCS):** the two axes above are the
*block* DSL (a trailing block fills structure). The *value-chain* primitive is
orthogonal: `x.f(args)` desugars to `f(x, args)` when `f`'s first parameter
matches `typeof(x)`, so calls chain left-to-right
(`expect(5).to_equal(5).to_be_gt(0)`). It works on imported library surfaces
and is a strict last-resort that never shadows a module/field call. See
[Fluent Method Chaining (UFCS)](#fluent-method-chaining-ufcs).

## Background

The builder-style DSL pattern, where nested blocks of code describe structure
declaratively while retaining full imperative power, originated in Smalltalk's
block-based APIs, was popularized by Ruby (Shoes, Sinatra, RSpec), and refined by
Groovy (SwingBuilder, MarkupBuilder). Kotlin (Compose, TornadoFX) and Swift (SwiftUI)
carry the tradition forward.

The defining characteristic: **functions accept trailing blocks that execute in the
context of the caller**, creating a nested, readable structure without explicit
parent-child wiring.

## Closure Syntax

Aether closures use pipe-delimited parameters with arrow or block bodies:

```aether,fragment
// Arrow closure (single expression, returns value)
doubler = |x: int| -> x * 2

// Block closure (multiple statements)
greeter = |name: string| {
    println("Hello ${name}")
}

// No-parameter closure
action = || {
    println("done")
}
```

Closures capture variables from their enclosing scope:

```aether,fragment
base = 100
adder = |x: int| -> x + base    // captures 'base'
result = call(adder, 42)         // 142
```

## Invoking Closures

Use the `call()` builtin:

```aether,fragment
doubler = |x: int| -> x * 2
result = call(doubler, 21)    // 42
```

## Closures as Function Parameters

Functions declare closure parameters with the `fn` type:

```aether
apply(x: int, f: fn) {
    return call(f, x)
}

apply_twice(x: int, f: fn) {
    return call(f, call(f, x))
}

main() {
    doubler = |x: int| -> x * 2
    println(apply(21, doubler))         // 42
    println(apply_twice(3, doubler))    // 12
}
```

## Trailing Blocks

Any function call can be followed by a block `{ ... }`. This is the foundation of
the builder DSL pattern:

```aether,fragment
setup("config") {
    println("initializing")
    // arbitrary code here
}
```

There are two forms with different semantics:

### Immediate blocks (DSL structure)

A bare `{ }` after a function call runs immediately, inline:

```aether,fragment
panel("title") {
    button("OK")       // runs now, during construction
    button("Cancel")
}
```

### Same-line rule for trailing blocks

A trailing block `{ ... }` is recognised as part of a function call only
when `{` appears on the **same source line** as the call's closing `)`.
A `{` on a later line is parsed as an independent block, not as a
trailing closure for the preceding call.

```aether,fragment
result = build() { ... }    // trailing closure of build()

result = build()
{ ... }                     // independent block, runs after build()
```

Why: this rule lets ordinary statements like

```aether,fragment
base_body = read_blob(repo, sha)
{
    n = string.length(base_body)
    // …
}
```

keep the obvious lexical scoping, the bare block sees `base_body`,
because it was never folded into `read_blob`'s argument list. Without
the same-line rule, the parser greedily consumed any `{` that followed
a call as a trailing closure (whether the call was an assignment RHS
or a statement), and the block body could not see the variable being
declared by the enclosing assignment.

When a `{` on a later line follows a call directly, the compiler emits
a warning suggesting the user move the `{` to the call's line if they
intended a trailing closure. This makes the typical foot-gun
(intending a closure but writing the brace on the next line)
self-diagnosing.

### Closure blocks (callbacks)

A `|| { }` or `|params| { }` after a function call creates a real closure that is
passed as an argument, it runs later when invoked:

```aether,fragment
save_handler = || { println("saved!") }
button("Save", save_handler)

// Later...
call(save_handler)    // prints "saved!"
```

This mirrors Groovy's `actionPerformed: { ... }` pattern.

### Callback blocks

The `callback` keyword before a trailing block creates a real closure (hoisted,
with variable capture) without requiring explicit parameters. This is the third
trailing-block mode:

| Mode | Syntax | Semantics |
|------|--------|-----------|
| Immediate | `func() { block }` | Runs inline as DSL structure |
| Closure | `func() \|x\| { block }` | Real closure with explicit params |
| Callback | `func() callback { block }` | Real closure, captures from scope |

```aether,fragment
counter = ref(0)

btn("increment") callback { ref_set(counter, ref_get(counter) + 1) }
btn("decrement") callback { ref_set(counter, ref_get(counter) - 1) }
```

The callbacks capture `counter` from the enclosing scope. No need to thread it
through as an explicit parameter. At the call site, `call(handler)` is enough.

Callback blocks also support explicit params and arrow bodies:

```aether,fragment
// With params, invoked as call(adder, 3, 4)
store(action) callback |a: int, b: int| { return a + b }

// Arrow body, shorthand for single-expression callbacks
store(action) callback |x: int| -> x * 2
```

## DSL Block Receiver Scoping

When a trailing closure is bound to a member-access call,
`receiver.method(args) { body }` bare-name function references
inside `body` fall back through the receiver's namespace before
erroring as undefined. Resolution order:

1. Block-local definitions (closure params, `let` bindings).
2. `<receiver>_<name>` looked up walking the scope chain.
3. The current file's scope (existing imports, top-level decls).
4. Otherwise: undefined-name error.

Pre-fix, every downstream project consuming an SDK module had to
write a two-line import preamble:

```aether,fragment
import bash
import bash (script, jobs, env)        // ← this companion line

main() {
    b = build.start()
    bash.test(b) {
        script("test_a.sh")             // resolves via the (script, jobs, env) tuple
        jobs(8)
    }
}
```

Post-fix, the second `import bash (...)` line is unnecessary,
bare-name calls inside the trailing block fall back to
`bash_script` / `bash_jobs` automatically:

```aether,fragment
import bash

main() {
    b = build.start()
    bash.test(b) {
        script("test_a.sh")             // resolves via the receiver fallback
        jobs(8)                         // (no companion import needed)
    }
}
```

Pure additive. Existing code with explicit selective imports
keeps working unchanged; existing qualified calls
(`bash.script("x")` inside the block) still resolve via the
`module.func` path; explicit local definitions still shadow the
receiver fallback.

Receiver resolution: the prefix is checked first as a visible
namespace (the `bash` case above); if not, the prefix's typed-value
struct name (e.g. `b: Builder` → receiver = `Builder`).

Nested DSL blocks stack naturally: `outer.method(o) { mark("o");
inner.method(i) { note("i") } }` resolves `mark` as
`outer_mark` and `note` as `inner_note` the inner block's
receiver wins for inner-namespace names; outer's receiver applies
to outer-namespace names by the natural scope-walk in
`lookup_symbol`.

## Higher-Order Functions

Closures enable functional patterns with standard library collections:

```aether
import std.list

// User-defined each, map, filter
each(l: ptr, f: fn) {
    n = list.size(l)
    for (i = 0; i < n; i++) {
        val, _ = list.get(l, i)    // list.get returns (value, err)
        call(f, val)
    }
}

map(l: ptr, f: fn) {
    result = list.new()
    n = list.size(l)
    for (i = 0; i < n; i++) {
        val, _ = list.get(l, i)
        list.add(result, call(f, val))
    }
    return result
}

filter(l: ptr, f: fn) {
    result = list.new()
    n = list.size(l)
    for (i = 0; i < n; i++) {
        val, _ = list.get(l, i)
        if call(f, val) != 0 { list.add(result, val) }
    }
    return result
}

main() {
    nums = list.new()
    list.add(nums, 1)
    list.add(nums, 2)
    list.add(nums, 3)
    list.add(nums, 4)
    list.add(nums, 5)

    each(nums) |x: int| { print("${x} ") }
    // 1 2 3 4 5

    doubled = map(nums) |x: int| -> x * 2
    // [2, 4, 6, 8, 10]

    big = filter(nums) |x: int| -> x > 2
    // [3, 4, 5]
}
```

## Builder Context Stack

When a function call has a trailing block, Aether automatically pushes the function's
return value onto a **builder context stack** before executing the block, and pops it
after. Library functions can access the current context via `builder_context()`.

This enables automatic parent-child wiring without the caller specifying parents:

```aether
import std.list

frame(title: string) {
    children = list.new()
    return children
}

// _ctx: ptr as first param means "auto-inject builder context"
panel(_ctx: ptr, title: string) {
    children = list.new()
    if _ctx != null { list.add(_ctx, children) }
    return children
}

button(_ctx: ptr, label: string) {
    if _ctx != null { list.add(_ctx, 1) }
}

main() {
    root = frame("App") {
        panel("Controls") {
            button("OK")
            button("Cancel")
        }
    }
    // root -> [panel_children -> [button, button]]
}
```

## Invisible Context Injection

The `_ctx: ptr` convention is the key to making builder DSLs feel declarative.
When a function's first parameter is named `_ctx` with type `ptr`:

1. **The parameter is hidden from callers**, it doesn't count toward arity
2. **Inside trailing blocks**, the compiler auto-injects `builder_context()` as the
   first argument
3. **Outside trailing blocks**, the function can still be called explicitly with a
   context value

This means the user writes:

```aether,fragment
frame("Address") {
    panel("Enter your address:") {
        label("Street:")
        textfield("Evergreen Terrace", 20)
        label("Number:")
        textfield("742", 5)
    }
    panel("Actions") {
        button("Save")
        button("Cancel")
    }
}
```

And the compiler generates:

```c
frame("Address");
_aether_ctx_push(frame_result);
{
    panel(_aether_ctx_get(), "Enter your address:");
    _aether_ctx_push(panel_result);
    {
        label(_aether_ctx_get(), "Street:");
        textfield(_aether_ctx_get(), "Evergreen Terrace", 20);
        // ...
    }
    _aether_ctx_pop();
    // ...
}
_aether_ctx_pop();
```

## Builder Functions, "Configure Then Execute"

Regular trailing blocks run the function first, then decorate the result. **Builder
functions** flip this: the block runs first to fill a configuration, then the function
executes with that configuration.

This is the second flavor of trailing-block function, toggled by the `builder` keyword
on the definition:

| | When does it run? | Block provides | Function provides |
|---------|-------------------------------|---------------------|------------------------|
| Regular | Function first, block second | Decoration/children | The container to fill |
| Builder | Block first, function second | Configuration | The action to perform |

### Defining a builder function

```aether,fragment
import std.map

builder compile(src: string) {
    // _builder is implicitly available, it's the config the block filled
    // It's null when called without a trailing block
    rel = ""
    if _builder != null {
        if map_has(_builder, "release") == 1 {
            rel = map_get_raw(_builder, "release")
        }
    }
    println("compiling ${src} with release=${rel}")
}
```

The `_builder` parameter is compiler-injected (like `_ctx` for builder functions).
The caller never sees it.

### Calling a builder function

```aether,fragment
// With trailing block, block fills config, then compile() runs
compile("Main.java") {
    set_release("21")
    set_lint("all")
}

// Without trailing block, _builder is null, zero-config
compile("Test.java")
```

The setter functions (`set_release`, `set_lint`) are regular DSL functions
with `_ctx: ptr` they work on whatever was pushed to the context stack. The
compiler creates the config object (currently a map), pushes it, runs the block,
pops, then calls the function with the filled config.

### Builder functions can return values

```aether,fragment
builder make_greeting(name: string): string {
    prefix = "Hello"
    if _builder != null {
        if map_has(_builder, "prefix") == 1 {
            prefix = map_get_raw(_builder, "prefix")
        }
    }
    return "${prefix}, ${name}!"
}

main() {
    g = make_greeting("Alice") {
        set_option("prefix", "Hi")
    }
    println(g)  // "Hi, Alice!"
}
```

### Choosing the config factory with `with`

By default, the compiler creates the config object via `map_new()`. The `with`
clause lets the SDK author specify any zero-argument factory function:

```aether,fragment
// Default, map_new
builder compile(src: string) { ... }

// List, ordered collection of flags
builder run_command(name: string) with list_new { ... }

// Custom builder, any user-defined factory
query_builder_new() {
    m = map_new()
    map_put_raw(m, "_type", "query")
    return m
}
builder execute_query(db: string) with query_builder_new { ... }
```

The factory just needs to be a zero-argument function returning `ptr`. The
trailing block's setter functions and the builder function body must agree on the
protocol, the compiler doesn't care what the object is, only that it can be
pushed to the context stack as `void*`.

### Generated code

For `compile("Main.java") { set_release("21") }`, the compiler generates:

```c
{
    void* _bcfg = map_new();         // 1. create config
    _aether_ctx_push(_bcfg);         // 2. push as context
    {
        set_release(_aether_ctx_get(), "21");  // 3. block fills config
    }
    _aether_ctx_pop();               // 4. pop
    compile("Main.java", _bcfg);     // 5. function runs with filled config
}
```

Compare with the regular trailing block pattern:

```c
_aether_ctx_push((void*)(intptr_t)frame("App"));  // 1. function runs
{
    panel(_aether_ctx_get(), "Controls");           // 2. block decorates
}
_aether_ctx_pop();                                  // 3. pop
```

## Ref Cells, Shared Mutable State for Closures

Aether heap-promotes a captured variable that a closure **assigns to**: the
enclosing binding and every closure capturing it then share one heap cell, so
`count = count + 1` inside a closure is visible to the outer scope and to
sibling closures (the Ruby/Groovy model). A simple named local needs no ref
cell to share mutable state this way. A capture the closure only reads stays a
cheap aliased copy.

**Ref cells** are for shared mutable state that isn't a plain captured local:
a value passed explicitly as a `ptr` argument to callbacks (UI event
handlers), threaded through a struct field, or handed across an actor
boundary:

```aether,fragment
count = ref(0)           // heap-allocated mutable cell
defer ref_free(count)

inc = |r: ptr| { ref_set(r, ref_get(r) + 1) }

call(inc, count)         // mutates shared state
call(inc, count)
println(ref_get(count))  // 2
```

Ref cells work because closures capture the pointer by value, all closures
holding the same pointer see the same heap location.

**API:**
- `ref(value)` create a ref cell (heap-allocated `intptr_t`)
- `ref_get(r)` read the value
- `ref_set(r, value)` write a new value
- `ref_free(r)` free the cell (or use `defer ref_free(r)`)

### Storing Closures in Collections

Closures are structs (not pointers), so they can't be stored directly in
`std.list`. Use `box_closure()` / `unbox_closure()` to heap-allocate:

```aether,fragment
import std.list

handlers = list.new()
action = |x: ptr| { ref_set(x, ref_get(x) + 1) }
list.add(handlers, box_closure(action))

// Later: retrieve and invoke
boxed = list.get(handlers, 0)
handler = unbox_closure(boxed)
call(handler, some_ref)
```

### Interactive Calculator Example

Combining ref cells, boxed closures, and the builder DSL:

```aether,fragment
num  = ref(0)
prev = ref(0)
op   = ref(0)

digit  = |n: ptr, d: int| { ref_set(n, ref_get(n) * 10 + d) }
set_op = |n: ptr, p: ptr, o: ptr, v: int| {
    ref_set(p, ref_get(n)); ref_set(n, 0); ref_set(o, v)
}

g = grid() {
    btn("7") |n: ptr, p: ptr, o: ptr| { call(digit, n, 7) }
    btn("+") |n: ptr, p: ptr, o: ptr| { call(set_op, n, p, o, PLUS) }
    btn("=") |n: ptr, p: ptr, o: ptr| {
        v = ref_get(n); pv = ref_get(p); ov = ref_get(o)
        if ov == PLUS  { v = pv + v }
        if ov == MINUS { v = pv - v }
        ref_set(n, v); ref_set(p, 0); ref_set(o, 0)
    }
}

// Event loop: press button → invoke its callback
handler = unbox_closure(list.get(list.get(g, 1), cur))
call(handler, num, prev, op)
```

Each button's behavior is declared alongside the button, not in a separate
dispatch table. The ref cells provide shared mutable state across all callbacks.

## Fluent Method Chaining (UFCS)

Everything above describes the **block** DSL, a function accepts a trailing
block that fills structure (`describe("x") { ... }`). UFCS adds the
orthogonal **value-chain** primitive: calling a method on a value and getting
a value back, so calls read left-to-right like prose
(`expect(5).to_equal(5).to_be_gt(0)`). This is the shape every fluent
assertion / query / builder API is built on. It composes with the
trailing-block forms but is distinct from them.

**The rule.** `x.f(args)` desugars to `f(x, args)` when `f` is a free
function whose **first parameter type matches `typeof(x)`**. No new
declaration syntax: any existing free function whose first parameter is the
receiver type is callable in method position.

```aether,fragment
struct Subject { v: int, ok: int }
expect(n: int) -> Subject { return Subject { v: n, ok: 1 } }
// returns the receiver, so calls chain (the fluent-interface contract)
to_equal(s: Subject, want: int) -> Subject {
    ok = s.ok
    if s.v != want { ok = 0 }
    return Subject { v: s.v, ok: ok }
}

expect(5).to_equal(5)            // -> to_equal(expect(5), 5)
expect(5).to_equal(5).to_be_gt(0) // chains: each step returns a Subject
```

The receiver can be any expression, a call result (`expect(5).to_*`), a
stored value (`s = expect(5); s.to_equal(5)`), or a pointer
(`c.bump()` → `bump(c)` for `c: *Counter`, mutating in place).

**Works across the import boundary.** The fluent surface is normally provided
by a *library* and chained in *consumer* code, a test framework's matchers,
a query API's combinators. `value.method()` resolves a `method` exported by
an imported module whose first parameter matches `typeof(value)`, honoring the
same visibility as a normal qualified `mod.method(value)` call:

```aether,fragment
import assert
r = assert.expect_int(5).to_equal(5).to_be_gt(0)  // matchers imported, chain local
```

**Resolution & precedence.** UFCS is a strict *last resort*: module-qualified
calls (`string.length(s)`), struct-field access, and function-pointer-field
dispatch all keep priority. A dotted call only falls back to UFCS when it
would otherwise be an "Undefined function", so nothing that compiled before
changes meaning. Same-file functions win over imported ones; a receiver whose
type doesn't match the candidate's first parameter declines cleanly (no
silent coercion). There is no codegen change: the rewritten `f(x, args)` is an
ordinary call, and a small by-value struct threaded through an N-step chain is
N register-width copies the C backend elides, fluent chains are cheap.

**Ambient state for a facade.** A fluent facade often carries a shared
"current context" cell (set by `init()`, read by the chained matchers). Use a
module-level `var` (which persists across the import boundary) or
`std.config` for that, see [Ref Cells](#ref-cells-shared-mutable-state-for-closures)
for the closure-capture case.

## Comparison with Other Languages

For the broader "why this shape exists" comparison against Lisp,
Smalltalk, Rust, and Zig, see
[`closure-lineage-and-runtime-tradeoffs.md`](design/closure-lineage-and-runtime-tradeoffs.md).

| Feature | Smalltalk | Ruby | Groovy | Aether |
|---------|-----------|------|--------|--------|
| Block/closure syntax | `[:x | x * 2]` | `{|x| x * 2}` | `{x -> x * 2}` | `\|x\| -> x * 2` |
| Trailing block | `do: [...]` | `method do ... end` | `method { ... }` | `method() { ... }` |
| Implicit receiver | `self` in block | `instance_eval` | Delegate | `_ctx: ptr` convention |
| Builder pattern | Cascades | Shoes, Sinatra | SwingBuilder | Trailing blocks + context stack |
| Fluent method chain | Cascades (`;`) | method chaining | method chaining | UFCS (`x.f()` → `f(x)`) |
| Callback storage | Block variables | Procs/lambdas | Closures | `fn` type + `call()` |
| Shared mutable state | Instance vars | `@variables` | Delegate fields | `ref()` cells |

## Implementation Notes

Closures compile to C as:
- A `_AeClosure` struct: `{ void (*fn)(void); void* env; }`
- Hoisted static functions: `_closure_fn_N(_closure_env_N* _env, params...)`
- Heap-allocated environment structs for captured variables
- NULL environment for zero-capture closures

The builder context stack is a simple C array:
- `_aether_ctx_push(void*)` / `_aether_ctx_pop()` / `_aether_ctx_get()`
- Maximum nesting depth: 64 levels
- Zero overhead when not used (the stack is static, no allocation)

Trailing blocks (parameterless `{ }`) are inlined at the call site, no closure
allocation, no function pointer overhead. They are pure syntactic sugar for
sequential code with automatic context management.

Ref cells compile to:
- `ref(val)` → `malloc(sizeof(intptr_t))` + store
- `ref_get(r)` → `*(intptr_t*)r`
- `ref_set(r, val)` → `*(intptr_t*)r = val`
- `ref_free(r)` → `free(r)`

Closure boxing:
- `box_closure(c)` → heap-allocates an `_AeClosure` struct, returns `void*`
- `unbox_closure(p)` → dereferences back to `_AeClosure`

Additional builtins for interactive programs:
- `read_char()` → `getchar()` (blocking single-character input)
- `raw_mode()` / `cooked_mode()` → terminal mode switching (Unix `termios`)
- `char_at(str)` → ASCII value of first character
- `str_eq(a, b)` → string equality (returns 0 or 1)
