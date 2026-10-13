# Aether Memory Management

Aether's memory model is **deterministic scope-exit cleanup**, not garbage collection.

The guiding principle:
> **Allocations visible at call site. Cleanup explicit and composable. `defer` is your primary tool.**

Allocations are visible at the call site, and there are no GC pauses.

### Why `defer`?

1. **Visible** -- you can see every allocation and its cleanup in the source
2. **Composable** -- works with any function, not just stdlib types
3. **Predictable** -- no special naming conventions, no hidden registry, no surprises
4. **Familiar** -- same pattern as Go's `defer` and Zig's `defer`

The one-line cost (`defer type.free(x)`) is the price for knowing exactly what your program does.

---

## The Actual Model

Aether uses two allocation mechanisms:

| Layer | What | When |
|-------|------|------|
| **Actor allocation** | Actor state + inline mailbox | One NUMA-aware allocation per actor, freed when the actor is destroyed |
| **Stdlib heap** | `map.new()`, `list.new()`, etc. | Freed via `defer` or explicit `.free()` call |

There is **no garbage collector**.

### Arena vs defer: when to use which

They solve different problems and are used in different layers:

| | Arena | defer |
|---|--------|--------|
| **What** | A region of memory; many small allocations share one region. One "free" destroys the whole region. | A language construct: run cleanup when leaving the current scope. |
| **Who uses it** | A general-purpose arena allocator backs bulk-allocation use cases such as `std.arena` and JSON parsing. You opt into it explicitly. The actor path does **not** use arenas. | **You** use it in Aether code for stdlib types (`list.new`/`list.free`, `map.new`/`map.free`) and FFI buffers. |
| **Lifetime** | Everything in the arena lives until the arena is destroyed in one shot. | The resource lives until scope exit; the deferred call runs then. |
| **Typical use** | Bulk allocation where many objects share one lifetime and are reclaimed together (e.g. all nodes of a parsed JSON document). | Any heap allocation you make: `items = list.new(); defer list.free(items);` so the list is freed when the function (or block) exits. |

---

## Stdlib Convention

All stdlib types follow one consistent naming pattern. In Aether you call them with dot syntax; the underlying C functions use underscores (`type_new`/`type_free`):

```
type.new()    -> allocates on the heap, returns a pointer (must be freed)
type.free(t)  -> frees the allocation
```

| Module | Constructor | Destructor |
|--------|-------------|------------|
| `std.map` | `map.new()` | `map.free(m)` |
| `std.list` | `list.new()` | `list.free(l)` |
| `std.string` | `string.new()` | `string.free(s)` |
| `std.dir` | `dir.list(path)` | `dir.list_free(l)` |

**Rule**: Any function whose name ends in `_new()` or `_create()` (at the C level) returns an allocated object. Its matching `_free()` is its destructor. In Aether, use `type.new()` and `type.free(x)`.

---

## The `defer` Pattern

Aether's primary memory management pattern is `defer`: allocate, immediately defer the free, then use the resource. Cleanup runs at scope exit in LIFO order.

```aether
import std.map

main() {
    m = map.new()
    defer map.free(m)

    map.put(m, "k", "v")
    print(map.get(m, "k"))
    print("\n")
    // map.free(m) runs here (scope exit)
}
```

This is explicit, visible, and composable. It works with any function -- not just stdlib types.

### Multiple allocations

```aether
import std.list
import std.map

main() {
    m = map.new()
    defer map.free(m)

    items = list.new()
    defer list.free(items)

    // Use both...
    // At scope exit: list.free(items) runs first (LIFO), then map.free(m)
}
```

### Returning allocated values

When a function allocates and returns a value, the caller receives ownership:

```aether
import std.list

build_items(n) : ptr {
    result = list.new()
    i = 0
    while i < n {
        list.add(result, i)
        i = i + 1
    }
    return result
}

main() {
    items = build_items(10)
    defer list.free(items)

    print(list.size(items))
    print("\n")
}
```

---

## Actor State

Actor `state` variables initialized with `*.new()` outlive any single message handler. Free them in the actor's `Stop` handler (or wherever the actor is shut down):

```aether,fragment
import std.map

message Store { key: string, value: string }
message Lookup { key: string }
message Stop {}

actor Cache {
    state data = map.new()

    receive {
        Store(key, value) -> {
            map.put(data, key, value)
        }
        Lookup(key) -> {
            print(map.get(data, key))
            print("\n")
        }
        Stop() -> {
            map.free(data)
        }
    }
}
```

The actor runtime frees the actor's single NUMA-aware allocation (the actor struct and its inline mailbox) when the actor is shut down. Stdlib allocations held in `state` fields are not part of that block, free them yourself, e.g. in the `Stop` handler above.

---

## Common Mistakes

**Forgetting `defer` after allocation:**

```aether,fragment
m = map.new()
map.put(m, "k", "v")
// LEAK: m is never freed
```

Fix: always pair allocation with `defer`:

```aether,fragment
m = map.new()
defer map.free(m)
```

**Deferring before the allocation succeeds:**

`defer` registers immediately. Place it right after the allocation, not before.

**Allocating inside a loop:**

`defer` is block-scoped: a `defer` placed inside a loop body fires at the end of
each iteration, not just once at function exit. So an allocation made inside the
loop is reclaimed every pass, no manual per-iteration free is needed:

```aether,fragment
while i < n {
    item = list.new()
    defer list.free(item)   // runs at the end of THIS iteration
    // ... use item ...
    i = i + 1
}
```

---

## Summary: When to Use What

| Situation | Approach |
|-----------|----------|
| Typical local allocation | `defer type.free(x)` right after allocation |
| Value returned from function | Caller defers the free |
| Value passed to an actor via `!` | Actor owns it; no defer in sender |
| Actor `state` initialized with `*.new()` | Free in `Stop` handler: `map.free(data)` |

---

## String memory model (heap-string-tracker)

Strings have a more granular model than other allocations: every reassignment to a string variable that has held a heap-allocated value frees the old buffer through a compiler-emitted wrapper. You don't write `defer string.free(s)` the compiler tracks ownership transitions automatically.

This follows the principle Bjarne Stroustrup laid out in [_How do I deal with memory leaks?_](https://www.stroustrup.com/bs_faq2.html#memory-leaks):

> ... successful techniques rely on hiding allocation and deallocation inside more manageable types. Good examples are the standard containers. They manage memory for their elements better than you could without disproportionate effort.

Aether takes the same shape, strings are the "standard container" for character data, and the compiler hides allocation and deallocation transitions behind the assignment operator. The user-visible model is "assign and reassign normally"; reclamation is the compiler's job.

### What gets freed automatically

For every string variable in a function, the compiler emits a companion `int _heap_<name>` tracker that's set to `1` after every heap-string assignment and `0` after every literal assignment. On reassignment, the wrapper `if (_heap_<name>) aether_heap_str_free(<old>)` decides whether to release the previous buffer.

```aether,fragment
s = ""                          // _heap_s = 0 (literal)
s = string.concat(s, "x")       // free(""): no, _heap_s was 0; _heap_s = 1
s = string.concat(s, "y")       // free(prev concat): yes, _heap_s was 1; _heap_s = 1
s = "literal end"               // free(prev concat): yes, _heap_s was 1; _heap_s = 0
```

The wrapper handles all four transitions (heap→heap, heap→literal, literal→heap, literal→literal) uniformly, so heap memory used by string-returning expressions is reclaimed eagerly without any user-visible `defer`.

The same tracker also covers two related cases automatically:

- **`release(s)` / `string.release(s)`** on a heap-tracked local lowers to
  a flag-guarded free (`if (_heap_s) { aether_heap_str_free(s); s = NULL;
  _heap_s = 0; }`). It reclaims both a refcounted AetherString
  (`string.from_int` / `new` / …) *and* a plain malloc'd `char*`
  (`string.concat` / `substring` / `to_upper` / `to_lower` / `trim`), and
  clears the tracker so the scope-exit free can't double-free. Safe on a
  literal, the flag is 0.
- A heap-returning string expression used directly as a `==`/`!=`
  comparison operand (`string.substring(s, i, j) == "lit"`) is captured in
  a temp and freed after the compare, the binary-operator analogue of
  the call-argument drain.

`*StringSeq` cons-cell locals get an analogous automatic lifecycle (freed
on reassignment and at scope exit via the refcount-decrementing
`string_seq_free`); see [sequences.md](sequences.md) § Automatic ownership.

All three trackings (strings, `*StringSeq` and `string?` locals) run in
every body that is a C function of its own: a function, a closure (#2761)
and a receive arm, the `after` arm included (#2748). A closure ran only the
string one and a receive arm none of the exit frees, so a `string?` or
`*StringSeq` local of a closure, and any such local of an arm, leaked what
it held on every call or message.

### `heap.new(T)` boxes that own string fields

`heap.new(T)` allocates a zero-initialised `*T` box; `heap.free(p)` reclaims
it. A struct with `string` fields is allowed (previously POD-only): the box
**owns** its string fields under the same tracker model described above. Each
string field carries a hidden `_heap_<field>` companion, so:

```aether,fragment
struct AppCtx { db: ptr; data_dir: string }

ctx = heap.new(AppCtx)                       // calloc, fields NULL, trackers 0
ctx.data_dir = string.concat("/var/", "data")// adopt heap string, _heap_data_dir = 1
ctx.data_dir = string.concat("/srv/", "x")   // frees the previous buffer first
ctx.data_dir = "literal"                      // borrowed: _heap_data_dir = 0
heap.free(ctx)                                // releases owned fields, then frees the box
```

A field store adopts a heap string and frees the previous value on
reassignment, exactly like a string variable. `heap.free(p)` routes through a
generated `<T>_heap_free` that releases every owned field before freeing the
box; a field last assigned a literal is borrowed and is never freed. This lets
a handler/server context own its config strings for its lifetime without
dropping back to a raw `malloc(...) as *T`.

A struct literal's `string` field takes a heap-tracked local the way an alias
of it does: when the literal is the local's last use, the string moves into
the field and the local's tracker is cleared; when the local is used again
(read, or freed with `string.free`), the field gets a copy and the local keeps
its own (#2602). Before, the literal always moved the string, so
`h = Holder { text: text }` followed by `string.free(text)` freed the string
`h` owned.

### `@scoped` bindings, checked non-escape

A `let`/`var` declaration can be annotated `@scoped` to declare that its value
**must not outlive the lexical block** that introduced it. This is opt-in
escape analysis, not a borrow checker (Aether has none). It turns a footgun
into a compile-time invariant and documents intent at the binding site:

```aether,fragment
process() -> long {
    @scoped buf = make_buffer(64 * 1024)
    return buf.checksum()        // ok, a scalar DERIVED from buf escapes
}

leak() -> *Buffer {
    @scoped buf = make_buffer(64 * 1024)
    return buf                   // compile error: the @scoped binding escapes
}
```

The typechecker rejects every way the binding's value could outlive the block:

- returning the binding,
- assigning/aliasing it into another binding or a struct field,
- placing it as an element of an array / struct / message literal,
- capturing it in a closure (closures lower to plain C functions, so their
  lifetime is statically known),
- inserting it into a container (`list.add`, `map.put`, `set.add`, …).

Only a value *derived* from the binding via a call (`buf.checksum()`,
`string.length(buf)`) may escape, that carries the call result, not the
binding. A value handed to a function that retains it beyond the call is
governed by that function's own contract; `@scoped` checks the direct escape
sites at the binding's own scope.

### Heap-string sources

The compiler treats these expressions as heap-allocated:

| Expression | Reason |
|---|---|
| `string.concat(a, b)` | Stdlib, always `malloc`'d |
| `string.substring(s, i, j)` / `string.substring_n(s, n, i, j)` | Stdlib, always `malloc`'d |
| `string.to_upper(s)` / `string.to_lower(s)` | Stdlib, always `malloc`'d |
| `string.trim(s)` | Stdlib, always `malloc`'d |
| `string_new_with_length(data, n)` | Stdlib, the length-aware AetherString constructor (`bytes.finish` is built on it) |
| `string.from_int` / `from_long` / `from_float` / `from_char` | Stdlib, each `string_new`s a fresh refcounted AetherString |
| String interpolation `"foo ${x}"` | Compiler-allocated via `_aether_interp`, a refcounted AetherString whose payload shares the header's allocation |
| User-defined `-> string` function whose body provably returns heap | Structural escape analysis (see below) |
| Tuple-returning function / `@heap` extern, per position | Per-position analysis (see below) |

**Interpolation produces the refcounted shape.** It used to hand back a plain
`malloc`'d buffer, and the runtime cannot free one of those: given a bare
`char*` it cannot tell a heap payload from a static literal, so `string.free`
no-ops on it. That was fine while the compiler's per-variable ownership flag
was in reach, and a leak the moment the value crossed a boundary where it was
not, such as a parameter of a helper that frees what it is handed (#1543).

The payload lives in the same allocation as the header, so this costs a larger
`malloc` rather than a second one: measured at +1.6% on a loop that does
nothing but interpolate two million strings, which is inside that benchmark's
run-to-run spread.

The stdlib entries above are hard-coded as intrinsic heap sources in the
codegen's `is_heap_string_expr` recognised by name regardless of how a
module declares the extern, so a `-> ptr` declaration of one of them can't
silently reintroduce a leak. The set is grown by audit: every runtime
function that mints a fresh owned buffer belongs here.

### User-defined `-> string` functions

A user-defined function that returns `string` is treated as heap-returning **iff *any* return statement in its body yields a heap-string-expression** (recursively considering other heap-returning user functions), an OR-fold across the return sites. A function whose returns are *all* string literals or forwarded borrowed parameters is NOT heap-returning, and the wrapper won't try to free its results. A function that *mixes* the two (one branch `return string.concat(...)`, another `return "constant"`) *is* heap-returning: its literal branches are duplicated into owned strings through the uniform-heap return wrap (see [Return-ownership contract](#return-ownership-contract-uniform-heap-return-escape) below) so the caller can free every branch identically.

A function written as several clauses (`label(n) when n > 0 -> ...`, `label(n) -> ...`) is a set of functions behind one name: each clause is a C function of its own, and the name is a dispatcher that tests each clause's patterns and guard in order and hands back what the matching clause returns (#2644). What the set hands over is decided over every clause (#2627): when one clause returns a heap string, every clause's literal or borrowed return goes through the wrap, and the default a set returns when no clause matches is an owned copy of the empty string, in a tuple at each owned string position, the other positions zero (#2646). Each clause gets everything a single function's body gets: its string locals' trackers and their exit frees, a cell for a parameter a closure mutates, struct and fixed-size array parameters of its own, its own contracts. A clause that keeps a `string` parameter takes a reference of its own on entry, as a single function does (copy-on-keep), and a caller, which cannot tell which clause a call reaches, frees what it passed unless some clause keeps the argument as it came. Any other question about a parameter (does the function free it, return it, store it) is answered for the set when any clause does. Before, only the first clause was read: a caller freed the literal another clause returned (a crash), leaked a heap result, or freed an argument a later clause had stored.

The answer is worked out once per function and remembered, so it reads the function's own body and nothing else: it is the same whichever function is being emitted when it is first asked. A local bound to another local (`t = x; return t`) is owned when the body binds `x` from a fresh string, or `x` is a `string` parameter the function keeps a reference of its own to (#2629). Before, the alias was judged by the tracker table of the function being emitted, so a function first asked about at a caller emitted before it was taken as returning a borrowed string, and every call leaked the alias.

```aether,fragment
my_concat(a: string, b: string) -> string {
    return string.concat(a, b)        // RHS is heap → my_concat returns heap
}

format_msg() -> string {
    return "constant"                  // RHS is literal → format_msg does NOT return heap
}

s = my_concat("a", "b")               // _heap_s = 1
s = my_concat(s, "c")                 // free(prev); _heap_s = 1
s = format_msg()                       // free(prev); _heap_s = 0, literal preserved
```

The recursive walk has cycle detection (mutual recursion through `-> string` user functions returns "not heap" conservatively, which is the safe answer when the structural analysis can't decide).

### Cross-block reassignment (the architectural piece)

`_heap_<name>` trackers are emitted at **function-entry scope**, not at the C scope where the variable is first assigned. This means a string variable first-assigned in one if-branch and reassigned in another, or first-assigned at the top of a function and reassigned inside a deeply-nested loop, has a tracker visible at every reassignment site:

```aether,fragment
result = ""                            // _heap_result = 0 at function entry
if cond1 {
    result = my_concat("a", "b")       // _heap_result = 1; tracker is at fn scope
} else if cond2 {
    result = my_concat("c", "d")       // free(""): no; _heap_result = 1
}
// `result` is heap-allocated here regardless of which branch ran;
// scope-exit cleanup uses _heap_result to free correctly.
```

Pre-fix, the second branch couldn't see the first branch's tracker (it was C-scoped to the first `if` body) and the build failed with `'_heap_result' undeclared`. The function-entry hoist closes that scope mismatch.

### Tuple destructures

The same wrapper fires when a heap string is unpacked from a tuple. For user-defined tuple-returning functions the compiler runs a **per-position structural escape analysis** that mirrors the single-value case. It walks every `return e0, e1, …` statement and **OR-folds** the heap-classification of each expression, per position: position `j` is heap-returning if *any* return-site's `j`-th expression is heap. Each heap-classified position is then routed through the same `aether_uniform_heap_str` wrap as a single-value return, so a position that is a fresh allocation on one branch and a borrowed literal on another (`return owned, n, ""` on success vs `return "", 0, "err"` on the error path, the shape of every `zlib` / `cryptography` / `lzf` decode function) still hands the caller a freeable pointer on *every* branch.

One case vetoes the OR-fold: a **whole-tuple passthrough** return, `return g(...)` where `g` is itself tuple-typed, forwards `g`'s tuple opaquely and cannot be wrapped position-by-position. Such a position is freeable only if `g` already guarantees it (`g`'s own per-position analysis); if `g` doesn't, the position is forced non-heap, so the caller is never told to free a value the passthrough left borrowed.

(Earlier releases AND-folded instead, a position counted as heap only if *every* return-site made it heap, which classified the mixed `decode`-shaped functions non-heap and leaked their success-path allocation at every caller. The OR-fold + uniform-heap wrap + passthrough veto replaced it.)

```aether,fragment
build_pair(prefix: string, name: string) -> (string, string) {
    return string.concat(prefix, name), string.concat(name, prefix)
    //     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^^^^^^^^^^^
    //     position 0 = heap            position 1 = heap
}

a = ""                              // hoisted; _heap_a = 0
b = ""                              // hoisted; _heap_b = 0
i = 0
while i < 1000 {
    a, b = build_pair("p_", "n_")   // wrapper fires per position:
    //                              // free(prev a) if heap; _heap_a = 1
    //                              // free(prev b) if heap; _heap_b = 1
    i = i + 1
}
```

Without the tuple-destructure wrapper the loop would leak 1000×2 heap allocations; with it, steady-state retention is two strings.

A `string?` position is always handed over owned by a named function or a closure, as a single `string?` result is (#2751, see Optional string returns below): each return puts there a string of its own (a fresh one or an owning local's as it is, a parameter, a literal or a field copied, `none` as nothing), and a whole tuple passed on from a C function or a closure the compiler cannot name is copied position by position. The local a destructure binds owns it, frees the string it replaces, and frees it at its scope end; a `_` slot frees it at once. A `string?` parameter such a function returns in a tuple is copied there, so it does not keep its caller's argument.

A slot that re-binds an optional local takes the slot's value as the optional, as `a = 7` does (#2794): a bare value is wrapped, and a `string` position into a `string?` local is owned by it when the call handed the string over, the local freeing the string it replaces. A slot that names a module-level `var`, with no local or parameter of that name declared before, writes the global, as `gx = value` does (#2796): the global keeps a string of its own (adopted when handed over, copied otherwise, never freed, as every string a global holds) and a reference of its own to a closure, giving back the one it held. An optional global takes the slot as the optional, a bare value wrapped, and a `string?` one a string of its own, keeping the one it held as every string global does, as its `g = v` re-bind does (#2800, see the language reference's module-level globals). In a receive arm, a slot that names a state field writes the state, as `field = value` does (#2806): a string field takes a string of its own and gives back the one it held, a `fn` field a reference of its own.

A closure (`fn`) position is handed over when every return puts there a closure nobody else holds (#2756): a closure literal, a call that hands one over, a local whose only escape is that return, or the same position of a tuple call passed on whole. The local a destructure binds then releases the closure's env at its scope exit, as a local bound to a single closure result does (#2494), and a rebinding releases the env it replaces; a closure cell takes a reference of its own and gives the slot's back, and a `_` slot gives it back at once. A position that lends its closure (a field the callee read, a parameter it hands back) is left to its owner; a plain local bound to a parameter handed back takes a reference of its own, as an alias does (#2763, see Closure environment lifetime). Before, no position was asked, and every call leaked the env.

### `@heap` / `@borrow` annotations on tuple-returning externs

For C externs there is no body to walk, so the compiler exposes per-position annotations:

```aether,fragment
extern decode_b64(b64: string) -> (string @heap, int, string)
//                                 ^^^^^^^^^^^^       ^^^^^^
//                                 fresh malloc       borrow / static literal
//                                 (auto-free)        (no auto-free)
```

Default for unannotated string positions is `@borrow` preserves the previous silent behaviour for existing tuple-returning externs. Adding `@heap` to a position is a behaviour change: the wrapper starts auto-freeing previous values on reassignment, so any caller currently doing manual `string.release` against the returned pointer must drop that call when adopting the annotation.

Mix-and-match is allowed; trailing positions default to borrow:

```aether,fragment
extern realpath_raw(path: string) -> (string @heap, int, string)
extern get_pair(s: string)        -> (string @heap, string @borrow)
```

### Stdlib externs annotated `@heap`

`@heap` is used across several stdlib externs, it appears today in `std.bytes`,
`std.io`, `std.regex`, `std.os`, `std.fs`, and `std.strbuilder`. Each is audited
and annotated so that its return (whether a single `string @heap` or an `@heap`
tuple position) auto-frees at function exit; you do **not** need to call
`string.release` on the resulting strings. Representative carriers:

| Extern | Heap position | Notes |
|---|---|---|
| `bytes.finish` (`aether_bytes_finish`) | result string | length-aware AetherString constructor |
| `strbuilder.finish` (`aether_strbuilder_finish`) | result string | caller owns the assembled buffer |
| `regex.replace` / `regex.replace_all` / capture get | result string | fresh malloc per call |
| `fs.realpath` (`fs_realpath_raw`) | resolved path | POSIX `realpath(NULL)` / Windows `GetFinalPathNameByHandleW` + UTF-8 malloc |
| `fs.read_binary` (`fs_read_binary_tuple`) | bytes | fresh buffer |
| `io.fd_read_n` / `io.fd_read_line` (tuple) | read buffer | fresh buffer per read |
| `os.run_capture_status` (`os_run_capture_status_raw`) | captured stdout | realloc-grown buffer |
| `os.run_pipe_drain_and_wait` (`os_run_pipe_drain_and_wait_raw`) | pipe payload | realloc-grown buffer |

Other tuple-returning string-position externs in the stdlib stay at default `@borrow` until a per-callee audit confirms heap-ness AND verifies no caller relies on manual `string.release` of the returned pointer.

### Function-exit defer-free for hoisted heap-string vars

The wrapper-on-reassignment frees the **previous** value when a heap-string variable is assigned to. The function-exit defer-free closes the complementary case: a heap-string variable assigned **once** and never reassigned still has a live allocation when the function exits, without an exit-time free, that allocation leaks per-call.

The codegen now emits `if (_heap_<name>) { aether_heap_str_free(<name>); <name> = NULL; _heap_<name> = 0; }` at every function-exit and explicit `return` for every hoisted heap-string variable that is **not** escaped. `aether_heap_str_free` dispatches on the AetherString magic header: `string_release` for refcounted AetherStrings, plain `free` for malloc'd `char*`. The escape walker (the same one the wrapper consults) decides which variables are held by something that outlives the call (return, closure capture, `ptr`-typed param, `@retain`-typed param, recursive escape via another store) and skips the defer for those.

```aether,fragment
foo(b64: string) -> int {
    raw = decode_b64(b64)         // _heap_raw = 1
    n   = check(raw)              // raw is not escaped (read-only)
    return n
    // function-exit: if (_heap_raw) aether_heap_str_free(raw);  ← reclaims the allocation
}
```

Cost: zero on functions that don't allocate heap strings; one inline conditional per non-escaped heap-string var per return path.

A C-style `for` loop's variable is owned as the same binding in the loop's `while` rewrite is (#2742). The loop declares it in a block of its own with the statement that binds it, and a step that binds a variable (`s = string.concat(s, "b")`, `i += 2`) is that binding at the end of each pass, so a string the step or the body replaces is freed as it is replaced, and the one the loop ends with (or a struct's owned strings) is freed when the block ends, by a `return` from inside the loop or a labeled exit of an outer loop too. A string variable is the loop's own, beside a local of its name that the loop shadows, and only the loop's own uses decide whether it escapes. Before, the init declared the variable in the C `for` header with no ownership flag and the step was a raw C assignment, so each replaced string and the last one leaked.

Such a loop variable, an owning `catch` binding (#2333) and the `some(v)` binding of a match over a `string?` (#2750) each live for one block and may shadow a local of their name. When the function names it anywhere else (a parameter, a local, a use), the binding is renamed, with every use inside its construct, to a name of its own (`ae_scoped<N>_<name>`) before code is generated (#2746). Each exit free then reaches its own variable: a `return` from inside the block frees the block's value and the shadowed local's. Before, both frees were keyed by the name, and inside the block the name was the block's, so the shadowed local's value leaked. A closure in the handler or the arm captures the binding as it captures any string local, with a reference of its own (#2769); it read an undeclared name before, unless the function had a local of the same name.

### `@retain` per-parameter annotation on extern declarations

For functions that *store* (or take a strong reference on) a string pointer beyond the call, such as refcount operations and owning map keys, the default "string parameter is read-only" treatment from the escape walker is wrong: it would let the function-exit defer-free reclaim the bytes while the recipient still holds the pointer, a use-after-free. The `@retain` annotation fixes this:

```aether,fragment
extern string_retain(str: @retain string)
extern map_put_string_owned(map: ptr, key: @retain string, value: ptr) -> int
```

Tells the heap-string-tracker escape walker "this slot stores the pointer; mark the heap-string arg as escaped, skip the function-exit free." Multiple annotations stack (`@aether @retain string` is legal; order doesn't matter). Default for unannotated string parameters remains "read-only", correct for `string.length` / `string.equals` / `print` / `println` and other consumers that don't outlive the call.

`@retain` is **only** correct when the callee keeps (or takes a strong reference on) the *caller's* pointer. A function that copies its argument is **not** a retainer: `map_put_raw` interns its key (`string_new(key)` at put time, released at `map_free`) and never holds the caller's key bytes after the call, so its `key` parameter is a plain `string` (borrowed), a heap key is reclaimed at the caller's scope exit. Marking such a copy-on-store parameter `@retain` leaks the caller's argument (the callee owns only its copy). The same goes for `string_list_add` / `string_list_set`: they **copy** the bytes into the list's own storage, so their `s` parameter is a plain `string` (borrowed), deliberately **not** `@retain` marking it would leak the caller's argument.

Audited stdlib retainers carrying `@retain` today: the refcount ops `string_retain` / `string_release` / `string_free` (`std.string`, each taking a strong reference on the same buffer), and the `key` parameter of `map_put_string_owned` (`std.collections`). Other functions are added as their callers are audited.

### `@noescape` per-parameter annotation on extern declarations

The opposite declaration, for closures (#2523). A closure passed to an extern `ptr` or `fn` parameter is a callback the C side may store and invoke later, so the compiler never releases its environment after the call unless the declaration says the function uses it only during the call:

```aether,fragment
extern string_seq_each(s: *StringSeq, f: @noescape ptr)
extern fs_walk_raw(path: string, cb: @noescape ptr) -> int
```

With the mark, the closure argument is treated as it is for an Aether callee whose parameter provably keeps nothing: a closure literal's environment is released right after the call (through its destructor, so the cells and strings it captured go too), a closure local keeps its reference and releases it at scope end, and a `ptr` slot gets a box on the caller's stack (`_aether_box_closure_in`) rather than a malloc'd one. The C function must neither store, free nor hand the box or the environment to another thread. Without the mark the environment is kept for the rest of the program (the safe direction: a leak, never a use after free), which is what an extern that stores the callback (`aether_observe`, `aether_worker_run`, the HTTP server handlers) relies on. Only valid on `ptr` and `fn` parameters.

### Return-ownership contract (uniform-heap return-escape)

Every function whose return type is `string` honours one rule:

> **The caller may always release the returned pointer exactly once, regardless of which return branch produced the value, regardless of whether the value originated as a literal, a heap allocation, or a heap-tracked local.**

Release it with `string.free` / `string.release`, or leave it to the scope-exit reclaim, both of which route through `aether_heap_str_free` and handle either physical shape. Not with a plain `free()`: a refcounted value would lose its payload.

The classifier OR-folds the function's return statements: a function is *heap-returning* if **any** return yields a heap expression (`string.concat`, interpolation, a heap-returning user fn, an `@heap` extern, or a bare identifier of a heap-tracked local). For such functions the codegen wraps every return value through a small inline helper:

```c
static inline const char* aether_uniform_heap_str(const char* s, int is_heap) {
    if (!s) return NULL;
    if (is_heap) return s;                  // fast path, already heap-owned
    /* AetherString-aware length probe (magic-header detect, ASan-clean)... */
    return string_new_with_length(data, n); // cold path, own the literal
}
```

The cold path mints a refcounted AetherString rather than a bare buffer, because a bare one is not something the caller can release: given a plain `char*` the runtime cannot tell a heap payload from static storage, so it no-ops and the value leaks. That was one leaked block per call at every error slot a caller dutifully freed. The shape is not new to callers, since the sibling branch of such a function already returns a `string.concat` result, which is the same shape.

The flag is resolved at compile time wherever possible:

- `1` (statically) for `string.concat`, interpolation, heap-returning user fn / extern.
- `_heap_<name>` (one runtime int load) for bare identifiers of heap-tracked locals.
- `0` (statically) for literals.

Heap branches pass straight through (~2 ns hot-path cost). Literal branches malloc-duplicate so the caller's unconditional `free()` is always correct. Pure-literal functions (every return is a literal) are not heap-classified and emit no wrap at all, zero overhead.

Internally, the escape walker now distinguishes two channels:

- **Container-escape** (call-arg, struct-field write, closure capture, `@retain` param): a recipient may have stored the pointer. The reassign-wrapper-free is suppressed; the function-exit defer-free is suppressed.
- **Return-only-escape** (the variable appears in a `return <name>;` and nowhere else): no recipient stashes the pointer. The reassign-wrapper-free **fires** so intermediate accumulator buffers are reclaimed at each loop iteration; the function-exit defer-free is still suppressed (otherwise it would dangle the return value, which the caller now owns). A `return` in a closure's body is the closure's own: the variable it names is the closure's capture, held by its env or shared cell, and does not escape the enclosing function through it (#2659). Counted as the function's, the function drained the variable at its own returns as a local it hands back, and for a cell a closure writes (`const char**`, declared inside a `for` body) the C did not compile.

The two-channel split closes the v0.149 "lucky-UAF return-escape" trade-off, the callee no longer frees the buffer the caller is about to read; ownership transfers cleanly through the uniform-heap helper.

#### Pass-through caveat

A function like `identity(s: string) -> string { return s }` where `s` is a parameter is classified non-heap (parameters are not tracked locals). The caller does not free. If the caller passes a heap value, the result is a borrow, the caller is responsible for the original allocation, not the return value. A fresh temporary passed to it is the exception: the call keeps it when it comes back, and the call's value is then the caller's, the temporary or a copy of whatever else came back (#2649, "Through a `string` parameter"). To force a heap copy across a pass-through boundary, use `return string.concat(s, "")` (or `string.concat("", s)`).

#### Optional string returns

A function returning `string?` hands its caller a string of its own whenever the result holds one, from every return (#2740): the caller binds the result as owned and frees it at its scope end. A fresh string, or a local's own, is handed over as it is; anything else (a parameter, a literal, an element of an array or a slice, a field) is copied through the uniform-heap helper above. An optional passed on is kept as it is when it comes from a call (which handed it over) or from a `string?` local that owns its string, and its string is copied otherwise (a `string?` parameter, `o ?? x`). So no return keeps a parameter: an argument passed to such a function stays its caller's, freed as usual. Before, a borrowed string was returned as it was, and the caller freed it: a parameter its own caller still held, or a literal. A `string?` position of a tuple result follows the same rule (#2751, see Tuple destructures above).

A result nobody binds is a temporary of the expression that uses it, which frees its string (#2750). `f()!` and `f() ?? d` yield a string of their own (the default `d` is copied unless it is a fresh string itself), which whatever reads it frees or takes as it does a `string.concat` result: a comparison, an interpolation, a call it is passed to, a local it is bound to. `f() == none` frees the string once the flag is read; `g(f())` frees it after the call when `g`'s parameter keeps nothing (it does not escape `g`, or only through a `string?` return, which copies it), and leaves it otherwise; `match f() { ... }` gives it to the `some` binding, which owns it for the arm as a local bound to a heap string does; `f()` thrown away is freed. This holds for a call of an Aether function; a closure's or a C function's `string?` is left as it is. Before, each of these leaked the string, one per evaluation.

A `string` function returning `w ?? d` or `w!` for a `string?` local hands its caller a copy of the local's string (#2815): the local owns that string and frees it at its scope end, which runs after the return value is read and before the caller sees it. Such a return makes the function heap-returning, so every other return of it is handed over too, a literal copied; a fresh default (`w ?? string.concat(a, b)`) is handed over as it is when it is the one taken. A `string?` parameter's string is its caller's and is returned as it is, unless a closure writes the parameter (then it is a cell holding its own copy, and the return copies it as well). The default of `w ?? d` is copied the same way when the function owns it: a local bound to a heap string, or a parameter the function captured because it escapes through that return (#2834). The `some(s)` binding of a match on a `string?` local is a copy the arm owns, as over a cell, so `return s` and a tuple's `return s, 1` hand it over (#2835). Before, the freed string was returned. The `some(s)` binding of a match on a `string?` the match owns (a call's result, a closure cell, a local) is the arm's own string, so `return s` hands it over, at a single or a tuple position, and the function is heap-returning for it too (#2823).

#### Recursive heap-returning functions

A function that calls itself recursively (e.g. a `walk_join`-style accumulator-passing recursion) is classified heap-returning *optimistically* on the recursive return. The cycle-break in `function_def_returns_heap_string` can't resolve "f returns heap iff f returns heap" during f's own analysis, so the walker treats `return f(...)` inside f's body as "heap", which is the conservative choice for the uniform-heap contract because the shim's cold path malloc-duplicates literal returns. Mis-classifying recursion as non-heap would cause UAF when the base case `return param` returns a buffer that the recursive wrap is about to free.

### Argument-temp lifetime (nested heap-returning calls)

`f(g(), h())` where `g()` and `h()` are themselves heap-returning was a pre-fix anonymous-temp leak: the two heap allocations flowed into `f` and had nowhere to be reclaimed. The codegen now wraps every call site that has heap-returning function-call subexpressions in argument position in a sequence of temporaries:

```c
char* _ad_0; char* _ad_1; <ret_type> _ad_r0;    /* ahead of the statement */
...
(_ad_0 = (char*)(g()),
 _ad_1 = (char*)(h()),
 _ad_r0 = f(_ad_0, _ad_1),
 aether_heap_str_free(_ad_0),
 aether_heap_str_free(_ad_1),
 _ad_r0)
```

It is a comma expression, and its temporaries are declared ahead of the statement it is in (#2765): plain C11, so a C compiler that is not GCC-compatible (cl.exe) takes it as well. It was a GNU statement expression `({ ... })`, which only GCC-compatible compilers take. Every other construct that needs a temporary in the middle of an expression (a string taken into an owning slot, an operand evaluated ahead to keep source order, the `!`, `??`, `?.` and `or` operators) is written the same way. An `or` with a block handler, a `!` that propagates an error, and a call whose trailing block runs where its value is used hold statements, so they run as statements ahead of their statement, its value left in a temporary the statement reads. Whatever the statement evaluates before one (an earlier argument, the left side of an operation, an earlier field) is evaluated into a temporary first, in source order (#2798); one behind `&&` or `||`, in a branch of an `if` expression, in the default of `??` or in an `or` handler runs under the same test, only when that part would; one in a loop condition runs at the top of every pass, and one in a `for` loop's increment where the increment runs. That covers every statement, a `match` arm's value and an actor state's initializer.

The wrap is suppressed when the corresponding callee parameter is storage-shaped (`ptr`, `@retain` string, or unknown-typed), the same `call_arg_escapes` gate the escape walker uses. In those cases the recipient takes ownership and freeing here would dangle the stored copy. A parameter the callee only returns (`ident(s) { return s }`) keeps the temporary alive for the result, which the caller then owns; in statement position the result is thrown away, so the temporary is freed with it.

A statement that throws away a string it owns frees it at once: a bare call that returns a heap string, a `string.concat` or an interpolation on a line of its own, an ask answered with a string, and `_ = e`.

### Closure arguments are borrowed

A call through an `fn` value (`call(f, mk(a))`, or `f(mk(a))` on an `fn` parameter) has no body for the compiler to read, so it cannot ask, as it does for a named function, whether the parameter keeps the argument. Closures follow a calling convention instead (#2499): **a closure borrows its arguments**, so the caller frees an owned argument after the call, through the same `_ad_N` wrap.

- A closure that keeps a `string` parameter past the call (stores it in a list, map or struct field, assigns it to a captured variable, passes it to a call that keeps it) takes a reference of its own when it is entered: a refcounted string is retained, a plain buffer copied. Capturing the parameter in a nested closure, or returning it from a string closure, already takes one (the env's capture, the uniform-heap return), so those need nothing more. Assigning it to a local is a keep only when that local keeps it; a local dropped at the call's end keeps nothing, and the reference would have nobody to give it back. A named function that keeps a `string` parameter takes its own reference the same way, in its own prologue, whether it is called by name or as a closure value (see the container section above).
- A closure's struct parameter follows a named function's rule (#2606): it borrows the caller's strings (its `_heap_<field>` trackers are cleared on entry, so the closure frees at its exit only the strings it stored into the parameter itself), unless the body keeps the parameter as a whole value, in which case it takes references of its own on entry and hands back strings of its own (#2582, see the statement temporaries below).
- A typed function pointer (`fn(...) -> R`, a C function pointer rather than a closure) follows the same convention (#2586), position by position. A `string` it returns is the caller's: a function used as a pointer that hands over owned strings marks the one it returns (`aether_fnptr_give`, a thread-local the runtime keeps), and the call takes a marked result as it is and copies any other, a C function's or a borrowed one (`aether_fnptr_take`), before it frees its argument temporaries. An owned `string` argument is freed after the call, and a parameter passed on through one is not kept by being passed, at each argument position where no function used as a pointer keeps what it receives there: a `string` parameter is kept when the function keeps it as it came (a return of it is not, as the call copies an unmarked result), and a `ptr` one (which a string can reach) by any escape. A parameter that shadows a function's name is not that function. The positions are settled together, to a fixed point, since a function can pass its parameter on through another pointer. Before, the caller took every result as borrowed, so a getter returning a field (a copy of its own since 0.792) leaked one string per call through the pointer.
- A `ptr` parameter cannot be copied: nothing knows what it points at. So the convention holds for a program only if no closure it can call keeps a `ptr` parameter (stores it, captures it, returns it), checked over every closure literal and every function used as a closure value. One such closure anywhere turns it off, and closure-call arguments are then left alone (a leak, never a free under a closure that kept the pointer).
- A closure whose body the compiler did not see could keep anything, so the convention is also off when one can be called: in a library build (`--emit=lib`); when an extern returns a closure, or returns or takes a struct with a closure field; when a C-laid-out struct has a closure field; when a `@c_callback` function takes a closure; when a `ptr` becomes a closure (`unbox_closure`, or a `ptr` passed to an `fn` parameter); and when a raw pointer is viewed as a struct with a closure field.

### Container value ownership (`map.put` / `list.add`)

A heap string stored as a container *value* is owned by the container and released when the element leaves it (`list.remove`, `list.clear`, a `map.put` over the key, `map.remove`, `map.free` / `list.free`) **only when the codegen can prove, at the put site, that the value is owned**:

- **A fresh heap value** (`string.concat(...)`, interpolation, a heap-returning call): the container adopts the single reference (`list_add_string_adopted` / `map_put_string_adopted`, no retain), so it is freed exactly once, by the container.
- **A heap-tracked local** (one some binding of the enclosing body gives a heap value), or a `string` parameter of a closure that keeps it by storing it (copy-on-keep, below): taken as every owning slot takes a local (`emit_string_take`): on the local's last use its reference moves into the container (`_heap_<name>` is cleared, so the local's scope exit frees nothing); read again afterwards, the container gets a copy of what the local owns, or holds its own reference to what it borrows (`list_add_string_owned` / `map_put_string_owned`), and the local keeps its own frees. Stored twice, or in a loop, the reference moves once and later stores copy; the same holds for a field read or an `if` over such values.
- **A literal**, a local that only ever holds a literal, or a value whose ownership can't be proven at the put site, stays on the non-owning `*_raw` path; the container never frees it.

**Through a `string` parameter: the callee keeps its own reference.** A storing wrapper (`add_name(l, s: string) { list.add(l, s) }`) keeps its parameter past the call, so, as a closure does (copy-on-keep, next section), it takes a reference of its own on entry (`aether_str_capture`: a refcounted string retained, a plain buffer copied) and the parameter is a heap-tracked local from there on: the store moves or copies it, a return hands it to the caller owned, and the exit frees what is left. The caller borrows whatever it passed: it frees a temporary after the call and keeps its own frees for a local, so the container owns its element in every shape and nothing frees it twice. This holds only when every keep is a slot the compiler releases (a container, a struct field, a cell, a callee that keeps the same way); a parameter that also reaches a sink nothing releases behind (an extern's `ptr` or `@retain` parameter, a callee without a body, a module-level `var`, which never frees what it holds, a field of a struct reached through a pointer not proven to be a `heap.new` box, such as `malloc(n) as *T`, which nothing destroys with its fields) is not captured, and the earlier rule stays: the container or global borrows and the caller keeps its argument alive. Nor is a parameter the function frees (`release_name(s: string) { string.free(s) }`, directly or through a helper that frees it): that is how a caller holding a string it does not track (a struct field, a string from C) hands it back, and a reference of the callee's own would be the one freed, leaving the caller's to nobody. A parameter inside what is returned, a field of a returned struct literal for instance (`cursor(t) -> Cursor { return Cursor { text: t } }`), is kept like any field store (#2584): the function takes its reference, the returned struct owns it, and the caller frees its own; only a bare `return t` hands the parameter's reference over. A call that only hands the parameter back (next paragraph) passes it on to wherever the call's value goes: returned (`greet(n) { return shout(n) }`), it is the return, and into a call that keeps nothing (`fs_make_temp_file_raw(dir, temp_prefix(prefix))`), it is no keep, so neither takes a copy; bound to a local, the local is followed as an alias is. A fresh temporary handed to such a call (`pass(k, string.concat(w, "!"))`) is kept when the call returns it and freed otherwise, whatever its shape, and the call's value is the caller's either way: the temporary itself, or a copy of whatever else came back (a literal, a local or parameter passed beside it), made before the temporaries are freed (#2649). Every consumer takes the call as a fresh string: a binding owns it, a return hands it over, a call it is passed to frees it after, a discarded one is freed, and an interpolation, a container or a struct field frees or adopts it; a local passed beside the temporary is never what comes out, so it keeps its own frees. Before, the temporary was kept when returned while every consumer took the value as borrowed, and nothing freed it. A captured parameter's copy is journaled for panic unwinding as soon as its exit free is armed, as a local's string is where it is assigned, so a panic through the body releases it too. A list or a map that adopts such a string takes it out of the journal with the adoption, so a panic after the store leaves it to the container: before, the panic's drain freed it while the container held it, and freeing the container freed it again (#2810). A stored string is therefore an AetherString as often as a plain buffer, so C that reads one out of a list or map goes through `aether_string_data`, as the `aether_config_*` accessors and the contrib host bridges do. The one caller-side question (`callee_keeps_string_arg`) is whether the caller's own pointer lives on past the call: never for a callee that captures the parameter; otherwise when the callee keeps it that way, or returns it as it is (a uniform-heap result is a copy). (Dispatching at runtime on the AetherString magic header would not do: it proves heap *representation*, not *ownership*, so a borrowed magic string still owned by another scope would be freed twice.)

**Through a call that hands its argument back.** A callee that returns a `string` parameter as it is and stores it nowhere (`pass(s: string) -> string { return s }`, or `fs`'s `temp_prefix`, which returns `"ae"` for an empty prefix and the prefix otherwise) yields its argument's pointer, or on another path something the argument's owner does not own. Every owning slot (a binding, a container store, a struct literal's field, an `if` or `match` arm of one) takes such a call of a heap-tracked local as it takes the bare local (#2548), unless the same call is also handed a fresh temporary, whose value is then a string of the caller's (above): when the pointer returned is the local's, the local's ownership moves to the slot on its last use, or is copied while the local is still read; otherwise the slot borrows. The local does not escape through the call, so its own frees stay in place. Before, the local was marked escaped and the slot held its buffer untracked, which leaked it once per call.

`list.set(l, i, v)` of a string owns it the same way: the element the slot held is released, a fresh value is adopted, a local is moved or copied, and any other string (a literal, a borrowed view) is copied, since only an owning set can release the old element. A raw pointer stored with `list.set` stays the caller's, and the old element is left where the caller put it (a sort or swap moves elements through `list.set`).

The key is always interned by the container (copied via `string_new`), so a heap key is the caller's to reclaim, see the `@retain` note above.

**Boxed closures.** A closure value (`fn`-typed, not a raw fn-pointer) stored into a list or a map is heap-boxed by the `fn -> ptr` coercion (`_aether_box_closure`), and `list.add(l, box_closure(f))` is that same store spelled with the box the coercion would make. The container owns the box and takes a reference of its own to the closure's env (`list_add_closure_owned` / `list_set_closure_owned` / `map_put_closure_owned`, #2518), released with the box when the element leaves; a closure literal or a handed-over call result stored this way is released right after the store, since the container's reference is the only one needed, and a local stored this way still releases its own. A bare function pointer (`is_fnptr`, a code address) is not heap and stays on the raw path.

The box is `{fn, env}` followed by a tag word. The `{fn, env}` prefix is the layout C code reads, so it stays first and unchanged: `std/collections` and `std/worker` mirror it, and the box pointer is still the `malloc` base that plain `free` reclaims. The tag is how `unbox_closure` tells a real box from a raw code address, which is what a bare function coerced into a `ptr` slot leaves behind. Unboxing one of those panics naming the cause instead of reading `env` out of the function's machine code and jumping through it. To make a bare function safe to unbox, pass it through an `fn`-typed parameter or call `box_closure` on it.

### Closure environment lifetime

A closure that captures variables carries them in a heap-allocated environment struct (`_aether_box_closure` / `_aether_make_env_N`). When such a closure *escapes*, stored in a list/map/struct, returned, or otherwise outliving the creating frame, the environment is reachable through the owner and reclaimed with it.

A **transient** capturing closure, created inline and passed to a function that only calls it and neither stores nor returns it (the callback shape `run(cb) { cb() }`), is dead once that call returns, so its environment is freed right after the call. The codegen emits the call in an env-draining form:

```c
{ _AeClosure _ad_0 = <closure>; run(_ad_0); if (_ad_0.env) _closure_env_N_free((void*)_ad_0.env); }
```

This fires only when the receiving parameter is proven not to escape the closure (`callee_param_escapes_via_body`): invoking a closure parameter (`cb()`, which lowers to an indirect-`call` node whose first child is the *callee*, not an argument) reads `cb.fn`/`cb.env` and runs it, it neither stores nor returns `cb` so the callee slot is not an escape, and passing it on to a function whose body only calls it (`it(cb) { it_impl(cb) }`) is none either: a visible body decides, as it does for every argument. If the callee *does* store or return the closure, or passes it to an extern parameter not declared `@noescape`, the drain is suppressed and the environment's lifetime follows the owner. The `if (_ad_0.env)` guard makes a zero-capture closure (NULL env) a no-op.

A capturing closure **bound to a local** (`g = || { ... }`) has its environment freed when the local's scope ends, on every exit (`return`, `break`, `continue` included), through the closure's generated destructor, which also releases the shared cells and strings the environment holds (#2480). A local rebound to a new closure literal, typically one per loop iteration, frees the environment it replaces. This happens only when every use of the local leaves no copy without a reference behind: calling it, passing it to a user function whose parameter is itself only called or passed on the same way, capturing it in a closure, or storing it into a list, map, struct field, message field, global or actor state, or binding another closure local to it, each of which takes a reference of its own. A local bound to another closure local or a parameter (`last = get`) retains it, so it owns that reference and releases it at its scope exit or its next binding, and the local it copied keeps releasing its own (#2668): before, the source stopped owning its value at the copy and the copy never started, so a closure made in a loop and kept in a local declared before it leaked one environment per pass. A parameter a function only aliases this way (`wrap(cb) { a = cb; call(a) }`) is therefore no keep of its caller's closure, which the caller releases as usual, a local or a literal alike (#2670), and a function returning such an alias (`a = b; return a`) hands its caller the alias's reference, which the caller then owns (#2671). A call of a function that hands its argument back as it is (every return gives back the same parameter, bare or through another such call, and nothing else keeps it: `keep(f) -> fn { return f }`) is the argument itself: a local bound to it (`g2 = keep(g)`), or a tuple slot it hands the argument back at (`g2, n = pair(g)` over `return f, 5`), is an alias that retains, the argument is judged where the call's value goes, as the name would be there, and `return keep(g)` returns `g` (#2763). Before, the argument counted as kept and the binding as a value someone else held, so neither local released it and every call leaked the environment. The argument is the one at the returned parameter's position, where a literal or `_` pattern parameter (`sel(0, a, b)`) takes a position too, and a call handing back a field or element read (`h = keep(o.cb)`) is that read, which the binding retains as `h = o.cb` retains. A closure cell passed to such a call keeps counting it as a keep: the call lends the cell's closure with a reference that ends with the call, and the call may rebind the cell, so the cell pins the closures it holds there, as before (#2788). All of these are judged in the callee's own scope (are the names it promotes to cells among them?), which is what its own emission finds, so the caller and the callee agree. A field's name is not a use of a local spelled the same (`Hooks { set: f }` or `h.set` beside a local `set`); taken for one, it kept the local's environment for good (#2669). Any other use (returning it, passing it to an extern parameter not marked `@noescape` or to a function that keeps it, or binding the local to anything but a fresh closure or another closure local) leaves the environment to whoever holds the value.

Environments are reference-counted (#2494). The closure value's owner holds one reference, and an environment that captures a closure takes one of its own and gives it back in its destructor, so a captured closure lives as long as either holder. A function whose every `return` hands back a closure nothing else holds (a literal, a local whose only escape is the return, or another such function's result) gives its reference to the caller, and a local bound to the call's result is freed by the caller's scope; passed to a call that keeps nothing, anywhere in an expression, it is freed after that call, and a discarded one at once (#2506, #2507). A local that hands its value on stops owning it right before the statement that does it (a condition, a loop condition or a `defer` included, #2507), and still frees the closures it is bound to afterwards (#2506). A struct that owns strings is captured as a copy with strings of its own, destroyed with the closure (#2504). A receive arm is a scope like a function body (#2498): its defers run and the closures, cells and struct locals it built are released when the handler ends, except a struct stored into actor state, whose strings stay with the state. Its string, `*StringSeq` and `string?` locals are freed then too (#2748), unless handed on; a store of a string local into a string state field (`label = msg`) gives the state a string of its own, the local's buffer on the local's last use (the local's flag cleared, and its unwind journal entry dropped) and a copy otherwise, so the local's own free never frees what the state holds, and two fields stored from one local each own a string. A send or a reply copies the strings it carries.

A closure kept in a struct's `fn` field, a message field, a module-level `var` or an actor's state is held by a reference of its own (#2525), on the same terms as a string a struct owns: the store takes one (`emit_closure_take`: a closure literal or a call handing one over is adopted, a view of a closure held elsewhere is retained through `_aether_closure_retain`), `<Name>_destroy` and `<Name>_replace` release the field's reference, `<Name>_dup` retains, and a struct parameter retains its closure fields at entry so its scope-exit destroy and any field store release its own, never the caller's. A fixed-size array field of such structs (`slots: Holder[2]`) is part of the value: each element is destroyed, replaced and copied with it, an element store takes the value, and an array literal of views copies each. A message's closure fields are released by `<Msg>_release_fields` with its strings; a state or global slot releases the env it held when rebound. A local bound to a field read (`x = h.cb`) retains and is released by its scope, and a named function returning a field read hands the caller a reference of its own, so neither outlives its holder by accident. A closure that captures a struct holding closures copies it with `<Name>_dup`, so the env's destroy releases references of its own. A struct a call returns by value, owning strings or closures, and that its statement keeps nowhere (the value thrown away, the argument of another call as in a fluent chain `expect_str(s).to_equal(t)`, or the object of a field read as in `n = string.length(make_item(w).name)`) is a temporary of that statement: it is held in a zeroed local and destroyed with `<Name>_destroy` once the statement is done, and on a `return` or `break` from inside the statement (an `or` handler). This holds for an expression statement, a declaration, an assignment, a tuple destructure and a `return`, and for an `if` or `while` condition (destroyed as soon as it is evaluated, each time), a `match` subject (once the match is done) and a `match` arm (#2582). A struct literal owning strings passed straight to a call of a function or closure the compiler emits (`reader(Item { name: string.concat(w, "old") })`) is such a temporary too (#2749): the callee borrows its struct parameter, so the literal's strings stay the statement's, and nothing destroyed them; the literal keeps what its fields hold (a struct a call returns into a field is the literal's, destroyed with it). An argument of a C function, an optional or sum parameter, or a struct pattern is left alone. What the statement keeps is not a temporary: the bound or returned value itself, an `if` arm or `or` default that becomes it, a struct literal's field, an array element. Nor is an argument the call's result may point into: one of a C function that returns a string, pointer or struct, or of a function whose `return` gives a parameter back as it came where its result can point into memory (`first(s, n) { return s }`, so `first(make_item(w).name, 1)`); a `string` result there, or each string position of a tuple, is copied where it is made, so the argument is still a temporary (#2619), while a pointer result, a tuple holding a pointer or an array, or one from C, keeps its argument alive rather than have it freed under the result. A closure returning a struct it captured hands back a copy (`<Name>_dup`), since its environment frees the captured one, and a struct assigned to a module-level `var` is taken (moved or copied), as a string is. A struct parameter borrows its caller's copy (its trackers are cleared on entry), unless the body keeps the parameter as a whole value (returns it, directly or in an `if` arm, a tuple or another struct; aliases, stores or captures it; uses a struct field of it as a value): then it takes references of its own on entry, a counted string retained and anything else copied, so what it hands back is its own and the caller destroys its argument as usual. A struct returned on one path is destroyed on the others: the mark that holds back a returned local's destroy is that `return`'s alone.

Four more holders follow the same rule (#2528). A `string[N]` struct field owns a heap copy of every element (as a string array cell does, #2474: a literal's elements are copied, a fresh heap string adopted, an element store frees what it replaces) and a `fn[N]` field a reference per element; a local array of structs that own strings or closures (`arr = [Holder { ... }, ...]`) destroys each element at scope exit, replaces an element on `arr[i] = v`, and copies each element on `other = arr`. An actor's state is released by `<Actor>_destroy_state`, the `destroy_state` hook on `ActorBase` that the generated spawn sets: the scheduler calls it exactly once from the free path that ends a scheduler-owned actor (a release once no reader can hold it, or the teardown), freeing each `string` state field per its tracker, which is now the actor's own `_heap_<name>` field rather than a handler local (so an overwrite in a later message frees the previous value), releasing each closure field's env and destroying each owning struct field. Assigning an owning struct state field replaces it as a local is replaced (`<Name>_replace`, #2579): the old value's strings are released and the new value is taken, a handler local's moved or copied, never shared. A reply message's owned fields belong to the asker once the reply arrives: the field the ask reads out is the asker's (a string copy, a closure reference the reply took), and the others are released with the reply buffer through `<Msg>_release_fields` (the MSVC ask helper applies the same rule through the release function it is handed); a reply with no taker, because the asker timed out or the message was sent rather than asked, is released by the replier (`scheduler_reply_owned`, which the reply statement calls with the message's release for a reply that owns anything). A `string[N]` or `fn[N]` state field owns its elements as the struct fields do, released by `destroy_state` and on an element store. A host that wants released actors ended before it goes on calls `scheduler_reclaim_released()`, which returns how many are still held back by a reader (0 once all are ended); the state's string tracker is spelt `self->_heap_<name>` at the state store sites only, so a handler local, parameter or closure of the same name keeps a tracker of its own. A closure literal passed to a function that stores it in a struct field, a message field, a global or state is released by the caller after the call, since the holder retained its own reference (the escape walk knows a store of a closure parameter keeps nothing of the caller's).

A local or parameter `string[N]` array owns its elements too (#2618), as a string array cell does (#2474): an element is a refcounted string the array holds a reference to, or a literal it holds as it is. An element store takes the value (a fresh string adopted, a view or a borrowed string copied, an owned plain buffer made refcounted) and frees the element it replaces; `arr = [...]` and `arr = other` take every new element before storing any, so `arr = [arr[1], arr[0]]` swaps; the scope exit frees what the array holds; and `string.free(arr[i])` frees the element and leaves it empty, so neither the next store nor the exit frees it again. Only an array some store into which is not a string literal pays for this: a table of literals (`t = ["zero", "one", "two"]`) is the plain C array it always was. An element of an owning array, of a `string[N]` parameter (its caller's elements), of a `string[N]` struct field or of a `string[N]` state field is a view, which a slot that keeps it copies, as it copies a struct's string field: a local bound to it, a value returned (directly, in an `if` or `match` arm, as a tuple position), a struct or container it is stored into, and the value of a call that hands its argument back as it came (`first(arr[0], 1)`, or `first(o.name, 1)` for a field). A `string[N]` parameter some store makes own something takes a reference to each of its caller's elements on entry and gives them back at its exit, so a store into it frees its own and never the caller's, and a closure that copies such an array, or a parameter array, into its environment takes a reference to each element, which the environment's destructor gives back. Before, a local string array owned none of its elements: a fresh string stored there leaked, and a view stored there dangled once its owner let go (`arr[0] = make_item(w).tag` kept a pointer into the statement's temporary). A struct a call returns, stored from as `o.f = make_item(w).tag` or `arr[i] = make_item(w).tag`, is a temporary of that statement as it is of an assignment (#2582); these stores were emitted before the statement's temporaries were opened, and the struct leaked.

A variable a `try` body writes is declared `volatile`, so that after a panic the catch, and the code after it, read what was stored before the panic: C leaves a local that changed between the `setjmp` a `try` lowers to and the `longjmp` of a panic indeterminate unless it is volatile, and at -O2 such a local holds whatever its register held before the `try`. The qualifier belongs to the variable itself (`const char* volatile s`, `const char* volatile arr[2]`); `volatile const char*` qualified the characters (#2618). Every kind of variable is covered, with the `_heap_<name>` flag that says whether a string one owns its string (#2690): a local wherever it is declared (at its first binding, at the function's entry for a string, optional string or `*StringSeq` local, ahead of the `if` or loop it is first bound in), a parameter (in the function's own signature, a closure's too), a message pattern binding of a receive arm, and a catch binding (#2739). A variable is volatile wherever a `try` nested deeper than its declaration writes it, so one declared inside a `try` body and written in a nested `try`, or a catch binding written in a `try` of its handler, is too; each function, closure and receive arm has its own set of such names. A struct value so declared (a local, a parameter, an element of such an array) is released and replaced through the generated `<Name>_destroy_volatile` and `<Name>_replace_volatile`, which take the volatile pointer and work on a copy. A fixed-size array so declared, passed to a `T[N]` parameter (a value the callee copies, #2516), is copied into an array of the call's own, read through a volatile pointer (#2723): the callee's plain `E _param_a[N]` would not take the volatile elements. Before, a parameter, a pattern binding, a catch binding and a local declared inside a `try` were plain: after a panic at -O2, a string parameter was its caller's string again while its flag said the function owned it, so the function freed its caller's string, and a catch binding lost what an inner `try` stored in it. A string stored inside a `try` into a local declared outside it is journaled for the frame the local is declared in, not the try's (see actor-concurrency.md, Heap cleanup on unwind): a panic the `try` catches leaves the string to the local, which frees it where its scope ends. Before, the panic freed it while the local still held it. A struct that owns strings and is declared ahead of the `if` it is first bound in is destroyed where it is declared, as one declared ahead of a loop is; it leaked the strings it held at the end.

A fixed-size array field bound from a whole array (#2688), in a struct literal (`Shelf { names: arr }`) or by a store (`s.names = arr`, `s.names = [...]`, and `names = arr` into an actor's state field), is filled element by element, each taken as the field keeps it: a `string[N]` field takes a reference to each element of another array (a refcounted string shared, anything else copied), as a parameter that owns its elements does, and a copy of each element of a literal; a `fn[N]` field takes a reference to each closure, and an array of structs that own strings a value of its own per element. A store takes every new element before it stores any, so `s.names = [s.names[1], s.names[0]]` swaps, then gives back each element it replaces. An actor's state field that is an array of structs owning strings or closures (`state Item[2] items`) owns its elements the same way: a whole-array store takes a `_dup` of each element and gives back each one it replaces, an element store and a store into an element's field replace what they overwrite, and `destroy_state` releases every element; it was copied as plain values, so the state shared strings its source then freed (`items = r.items`) and leaked the ones it replaced. Any other array field copies the elements as they are. These shapes did not compile before: C neither initialises nor assigns an array from an array. The source has the field's length (a type error otherwise, naming both); an array literal may be shorter, the rest zeroed, but not longer. A slice or a pointer has no length known before the program runs, and binding one to an array field is a type error: copy its elements one by one. A source of another length was accepted, and the copy read past the end of a shorter one. A store into a string field of an element of an array of structs (`items[i].name = v`, `o.items[i].name = v`) frees the string it replaces and records whether the struct owns the new one, as a store into a struct value's field does; it was a plain store, so a literal stored over an owned string was freed with the array and a fresh string leaked.

An element of a `string[]` slice is a view too (#2689): a slice owns none of its elements, and what does is not known where it is read (an owning array, a struct's field, a `make` buffer, C memory). A slot it escapes to takes a copy of its own: a value returned (directly, in an `if` or `match` arm, as a tuple position), a struct field, a container, and the value of a call that hands its argument back. Before, a function returning `xs[0]` of a slice handed the string back borrowed, and its caller held it after the array let go of it. A local bound to a slice element (`t = s[i]`) is not copied: it is a view that borrows the element, as a local bound to any borrowed string does, so code that moves elements through a local (`t = s[0]; s[0] = s[1]; s[1] = t`, as `std.sort` does) moves the pointers and keeps none. The view is valid while the element is: once the array replaces or frees it (`arr[0] = ...`, the array's scope exit, `string.free`), the local is a dangling pointer, so read it before then, or keep a copy (`string.copy`). Where the local escapes in turn it is copied, since it does not own what it holds: returned, it hands its caller a copy, and added to a container, the container takes a reference of its own. A struct field set from the local borrows the element as the local does, as from any local that borrows its string; set the field from the element itself (`Item { name: s[i] }`), which copies it, or from a copy. A store into a slice element is a pointer store: the slice owns none of its elements and does not know whether its storage does, so the store gives back nothing for the element it replaces, and what it stores becomes the storage's, a borrowed string as it is and a string the program owns (a fresh one, or a local that owns its string) handed over, which the local then never frees. Whoever owns the storage frees what its slots hold when it lets go: an owning array its elements at its exit, a `make` buffer's owner each element by hand. Moving elements through a local view (above) keeps the strings the storage holds as they were, one in each slot. Moving one through a copy does not: in `t = get(s, i); s[i] = s[j]; s[j] = t`, where `get` returns `s[i]`, `t` holds a copy of its own (a return copies an element), `s[i] = s[j]` drops the slot's string without giving it back, and `s[j] = t` hands the storage the copy, so the string replaced leaks, once per move. Read the element into a local (`t = s[i]`) to move it.

### When you DO need explicit cleanup

Strings returned from a function whose ownership the compiler can't infer (e.g. an opaque C extern returning `char*` without an `@heap` annotation) need the usual `defer free(s)` pattern, same as any other heap allocation. The automatic tracker covers in-Aether assignments, annotated extern returns, and non-escaped function-scope locals.

A field store frees the previous string whenever the field's `_heap_<field>` tracker says the struct owns it, wherever the pointer to the struct came from: a `heap.new(T)` box, a call's result, a list element cast back with `as *T`, a cast of a `ptr` parameter, a parameter of a function C may call (#2369). So does a store into a struct-valued field, a closure field or an element of a `string[N]` field through such a pointer. Reading the tracker is sound because every box Aether code allocates starts with its trackers zeroed: `heap.new(T)` allocates with `calloc`, and so does a call of C's `malloc` (declared under any Aether name, `extern malloc(n)` or `@extern("malloc") c_malloc(n)`), which is lowered to `calloc(1, n)` whether its pointer is cast at once (`malloc(n) as *T`) or bound to a `ptr` and cast later (#1873 was a store freeing the garbage tracker of a `malloc`'d box). Memory that reaches a `*T` from elsewhere (the part `realloc` adds, an arena, a C allocator) must be zeroed before a field is stored through it, as it must before `heap.free` destroys it. Before, only a pointer the compiler could trace back to `heap.new` released the old value, and a store through any other leaked it; freeing the old value by hand first (`string.free(p.name)`, or `p.name = set_owned(p.name, v)` with a `set_owned` that frees its first argument) still frees it once, since the field gives up what it is handed to a free (next paragraph). A field freed with C's `free()` is not given up that way: free a string field with `string.free`.

A field handed to a call that frees it gives its reference up (#2566). `string.free(p.name)`, `string.release(p.name)` and `release(p.name)` free the value (through the tracker where it can be read, as for a local) and leave the field empty and owning nothing. Passing `p.name` to a function that frees its parameter, such as `p.name = set_owned(p.name, v)` with `set_owned` freeing its first argument, clears the field's tracker at the call, so neither the store that follows nor the struct's destructor frees the same string again; that includes a field of a struct a call returned, which the statement keeps in a temporary and destroys (#2576). Such a function frees through the runtime, which releases a counted string and leaves a plain `malloc`'d buffer (an `@heap` extern's result) alone, so a plain buffer stays the field's, and the field frees it.

A field handed to a function that keeps its parameter as it was given (stores it past the call where nothing releases it, such as a module-level `var`, and takes no reference of its own) gives it up too, whatever the string is: the field's tracker is cleared at the call and the function's copy of the pointer outlives the struct, as a local handed to such a function is not freed at its scope exit (#2843). Before, the struct's destructor freed what the function kept, at once for a struct a call returned (`keep(make_item(w).name)`).

---

## Examples

See the following runnable examples:

- [examples/basics/memory_defer.ae](../examples/basics/memory_defer.ae) -- defer pattern (recommended)
- [examples/basics/memory_manual.ae](../examples/basics/memory_manual.ae) -- manual free pattern
- [examples/basics/memory_escape.ae](../examples/basics/memory_escape.ae) -- returning allocated values
