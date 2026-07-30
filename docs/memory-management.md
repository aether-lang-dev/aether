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

```aether
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

```aether
m = map.new()
map.put(m, "k", "v")
// LEAK: m is never freed
```

Fix: always pair allocation with `defer`:

```aether
m = map.new()
defer map.free(m)
```

**Deferring before the allocation succeeds:**

`defer` registers immediately. Place it right after the allocation, not before.

**Allocating inside a loop:**

`defer` is block-scoped: a `defer` placed inside a loop body fires at the end of
each iteration, not just once at function exit. So an allocation made inside the
loop is reclaimed every pass, no manual per-iteration free is needed:

```aether
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

For every string variable in a function, the compiler emits a companion `int _heap_<name>` tracker that's set to `1` after every heap-string assignment and `0` after every literal assignment. On reassignment, the wrapper `if (_heap_<name>) free(<old>)` decides whether to release the previous buffer.

```aether
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

### `heap.new(T)` boxes that own string fields

`heap.new(T)` allocates a zero-initialised `*T` box; `heap.free(p)` reclaims
it. A struct with `string` fields is allowed (previously POD-only): the box
**owns** its string fields under the same tracker model described above. Each
string field carries a hidden `_heap_<field>` companion, so:

```aether
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

### `@scoped` bindings, checked non-escape

A `let`/`var` declaration can be annotated `@scoped` to declare that its value
**must not outlive the lexical block** that introduced it. This is opt-in
escape analysis, not a borrow checker (Aether has none). It turns a footgun
into a compile-time invariant and documents intent at the binding site:

```aether
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
| String interpolation `"foo ${x}"` | Compiler-allocated via `_aether_interp` |
| User-defined `-> string` function whose body provably returns heap | Structural escape analysis (see below) |
| Tuple-returning function / `@heap` extern, per position | Per-position analysis (see below) |

The stdlib entries above are hard-coded as intrinsic heap sources in the
codegen's `is_heap_string_expr` recognised by name regardless of how a
module declares the extern, so a `-> ptr` declaration of one of them can't
silently reintroduce a leak. The set is grown by audit: every runtime
function that mints a fresh owned buffer belongs here.

### User-defined `-> string` functions

A user-defined function that returns `string` is treated as heap-returning **iff *any* return statement in its body yields a heap-string-expression** (recursively considering other heap-returning user functions), an OR-fold across the return sites. A function whose returns are *all* string literals or forwarded borrowed parameters is NOT heap-returning, and the wrapper won't try to free its results. A function that *mixes* the two (one branch `return string.concat(...)`, another `return "constant"`) *is* heap-returning: its literal branches are malloc-duplicated through the uniform-heap return wrap (see [Return-ownership contract](#return-ownership-contract-uniform-heap-return-escape) below) so the caller can free every branch identically.

```aether
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

```aether
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

```aether
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

### `@heap` / `@borrow` annotations on tuple-returning externs

For C externs there is no body to walk, so the compiler exposes per-position annotations:

```aether
extern decode_b64(b64: string) -> (string @heap, int, string)
//                                 ^^^^^^^^^^^^       ^^^^^^
//                                 fresh malloc       borrow / static literal
//                                 (auto-free)        (no auto-free)
```

Default for unannotated string positions is `@borrow` preserves the previous silent behaviour for existing tuple-returning externs. Adding `@heap` to a position is a behaviour change: the wrapper starts auto-freeing previous values on reassignment, so any caller currently doing manual `string.release` against the returned pointer must drop that call when adopting the annotation.

Mix-and-match is allowed; trailing positions default to borrow:

```aether
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

```aether
foo(b64: string) -> int {
    raw = decode_b64(b64)         // _heap_raw = 1
    n   = check(raw)              // raw is not escaped (read-only)
    return n
    // function-exit: if (_heap_raw) aether_heap_str_free(raw);  ← reclaims the allocation
}
```

Cost: zero on functions that don't allocate heap strings; one inline conditional per non-escaped heap-string var per return path.

### `@retain` per-parameter annotation on extern declarations

For functions that *store* (or take a strong reference on) a string pointer beyond the call, such as refcount operations and owning map keys, the default "string parameter is read-only" treatment from the escape walker is wrong: it would let the function-exit defer-free reclaim the bytes while the recipient still holds the pointer, a use-after-free. The `@retain` annotation fixes this:

```aether
extern string_retain(str: @retain string)
extern map_put_string_owned(map: ptr, key: @retain string, value: ptr) -> int
```

Tells the heap-string-tracker escape walker "this slot stores the pointer; mark the heap-string arg as escaped, skip the function-exit free." Multiple annotations stack (`@aether @retain string` is legal; order doesn't matter). Default for unannotated string parameters remains "read-only", correct for `string.length` / `string.equals` / `print` / `println` and other consumers that don't outlive the call.

`@retain` is **only** correct when the callee keeps (or takes a strong reference on) the *caller's* pointer. A function that copies its argument is **not** a retainer: `map_put_raw` interns its key (`string_new(key)` at put time, released at `map_free`) and never holds the caller's key bytes after the call, so its `key` parameter is a plain `string` (borrowed), a heap key is reclaimed at the caller's scope exit. Marking such a copy-on-store parameter `@retain` leaks the caller's argument (the callee owns only its copy). The same goes for `string_list_add` / `string_list_set`: they **copy** the bytes into the list's own storage, so their `s` parameter is a plain `string` (borrowed), deliberately **not** `@retain` marking it would leak the caller's argument.

Audited stdlib retainers carrying `@retain` today: the refcount ops `string_retain` / `string_release` / `string_free` (`std.string`, each taking a strong reference on the same buffer), and the `key` parameter of `map_put_string_owned` (`std.collections`). Other functions are added as their callers are audited.

### Return-ownership contract (uniform-heap return-escape)

Every function whose return type is `string` honours one rule:

> **The caller may always `free()` the returned pointer exactly once, regardless of which return branch produced the value, regardless of whether the value originated as a literal, a heap allocation, or a heap-tracked local.**

The classifier OR-folds the function's return statements: a function is *heap-returning* if **any** return yields a heap expression (`string.concat`, interpolation, a heap-returning user fn, an `@heap` extern, or a bare identifier of a heap-tracked local). For such functions the codegen wraps every return value through a small inline helper:

```c
static inline const char* aether_uniform_heap_str(const char* s, int is_heap) {
    if (!s) return NULL;
    if (is_heap) return s;                  // fast path, already heap-owned
    /* AetherString-aware length probe (magic-header detect, ASan-clean)... */
    char* dup = malloc(n + 1);              // cold path, dup the literal
    memcpy(dup, data, n); dup[n] = '\0';
    return dup;
}
```

The flag is resolved at compile time wherever possible:

- `1` (statically) for `string.concat`, interpolation, heap-returning user fn / extern.
- `_heap_<name>` (one runtime int load) for bare identifiers of heap-tracked locals.
- `0` (statically) for literals.

Heap branches pass straight through (~2 ns hot-path cost). Literal branches malloc-duplicate so the caller's unconditional `free()` is always correct. Pure-literal functions (every return is a literal) are not heap-classified and emit no wrap at all, zero overhead.

Internally, the escape walker now distinguishes two channels:

- **Container-escape** (call-arg, struct-field write, closure capture, `@retain` param): a recipient may have stored the pointer. The reassign-wrapper-free is suppressed; the function-exit defer-free is suppressed.
- **Return-only-escape** (the variable appears in a `return <name>;` and nowhere else): no recipient stashes the pointer. The reassign-wrapper-free **fires** so intermediate accumulator buffers are reclaimed at each loop iteration; the function-exit defer-free is still suppressed (otherwise it would dangle the return value, which the caller now owns).

The two-channel split closes the v0.149 "lucky-UAF return-escape" trade-off, the callee no longer frees the buffer the caller is about to read; ownership transfers cleanly through the uniform-heap helper.

#### Pass-through caveat

A function like `identity(s: string) -> string { return s }` where `s` is a parameter is classified non-heap (parameters are not tracked locals). The caller does not free. If the caller passes a heap value, the result is a borrow, the caller is responsible for the original allocation, not the return value. To force a heap copy across a pass-through boundary, use `return string.concat(s, "")` (or `string.concat("", s)`).

#### Recursive heap-returning functions

A function that calls itself recursively (e.g. a `walk_join`-style accumulator-passing recursion) is classified heap-returning *optimistically* on the recursive return. The cycle-break in `function_def_returns_heap_string` can't resolve "f returns heap iff f returns heap" during f's own analysis, so the walker treats `return f(...)` inside f's body as "heap", which is the conservative choice for the uniform-heap contract because the shim's cold path malloc-duplicates literal returns. Mis-classifying recursion as non-heap would cause UAF when the base case `return param` returns a buffer that the recursive wrap is about to free.

### Argument-temp lifetime (nested heap-returning calls)

`f(g(), h())` where `g()` and `h()` are themselves heap-returning was a pre-fix anonymous-temp leak: the two heap allocations flowed into `f` and had nowhere to be reclaimed. The codegen now wraps every call site that has heap-returning function-call subexpressions in argument position with a GCC statement-expression that:

```c
({ const char* _ad_0 = g();
   const char* _ad_1 = h();
   <ret_type> _ad_r = f(_ad_0, _ad_1);
   free((void*)_ad_0);
   free((void*)_ad_1);
   _ad_r; })
```

The wrap is suppressed when the corresponding callee parameter is storage-shaped (`ptr`, `@retain` string, or unknown-typed), the same `call_arg_escapes` gate the escape walker uses. In those cases the recipient takes ownership and freeing here would dangle the stored copy.

### Container value ownership (`map.put` / `list.add`)

A heap string stored as a container *value* is owned by the container and released when the container is freed (`map.free` / `list.free`) **only when the codegen can prove, at the put site, that the value is a fresh owned heap allocation**:

- **Statically heap** (`string.concat(...)`, interpolation, a heap-returning call, or a local proven heap-assigned in the enclosing body): routed to the owning variant (`map_put_string_owned` / `list_add_string_owned`, which adopt the caller's single reference, no retain, so the refcount balances at free time). The escape walker marks the same value escaped, so the caller doesn't also free it; the value is freed exactly once, by the container.
- **A literal**, or a value whose ownership can't be proven at the put site, stays on the non-owning `*_raw` path; the container never frees it.

**Ownership through a `string` parameter, the container borrows.** A value reaching a container *through a `string` parameter* of a storing wrapper (`pr(m, k, v) { map.put(m, k, v) }`) is left on the **non-owning** path: the put site sees only a `string` param, which can hold either an owned heap string the caller minted *or* a borrowed/literal one, and the two are indistinguishable there. The container therefore **borrows** such a value (it does not free it at teardown), and the **program** owns and frees it. (Dispatching at runtime on the AetherString magic header is *not* a sound shortcut: the magic header proves heap *representation*, not caller-transferred *ownership*, so a borrowed magic string still owned by another scope would be double-freed. Representation is not ownership, and a double-free is worse than a leak.) This matches the proxy/opts-map idiom (`std.map` holding both owned and borrowed values): retrieve the heap values via their owning handles and `string.free` them before `map.free`.

The key is always interned by the container (copied via `string_new`), so a heap key is the caller's to reclaim, see the `@retain` note above.

**Boxed closures.** A closure value (`fn`-typed, not a raw fn-pointer) stored into a list is heap-boxed by the `fn -> ptr` coercion (`_aether_box_closure`). The list takes ownership of the box (it is stored owned, and `list_free` reclaims the non-magic box via `free`); a bare function pointer (`is_fnptr`, a code address) is not heap and stays on the raw path.

### Closure environment lifetime

A closure that captures variables carries them in a heap-allocated environment struct (`_aether_box_closure` / `_aether_make_closure_N`). When such a closure *escapes*, stored in a list/map/struct, returned, or otherwise outliving the creating frame, the environment is reachable through the owner and reclaimed with it.

A **transient** capturing closure, created inline and passed to a function that only calls it and neither stores nor returns it (the callback shape `run(cb) { cb() }`), is dead once that call returns, so its environment is freed right after the call. The codegen emits the call in an env-draining form:

```c
{ _AeClosure _ad_0 = <closure>; run(_ad_0); if (_ad_0.env) free((void*)_ad_0.env); }
```

This fires only when the receiving parameter is proven not to escape the closure (`callee_param_escapes_via_body`): invoking a closure parameter (`cb()`, which lowers to an indirect-`call` node whose first child is the *callee*, not an argument) reads `cb.fn`/`cb.env` and runs it, it neither stores nor returns `cb` so the callee slot is not an escape. If the callee *does* store or return the closure, the drain is suppressed and the environment's lifetime follows the owner. The `if (_ad_0.env)` guard makes a zero-capture closure (NULL env) a no-op.

### When you DO need explicit cleanup

Strings returned from a function whose ownership the compiler can't infer (e.g. an opaque C extern returning `char*` without an `@heap` annotation) need the usual `defer free(s)` pattern, same as any other heap allocation. The automatic tracker covers in-Aether assignments, annotated extern returns, and non-escaped function-scope locals.

Raw-pointer structs reached through a cast (`p as *Slot`, with `malloc`/`free`) are outside the field heap-string tracker, a field write through a raw cast is a plain store with no reassign-free. Manage those fields' strings explicitly (free the old value before overwriting; free fields before the struct), exactly as in C.

---

## Examples

See the following runnable examples:

- [examples/basics/memory_defer.ae](../examples/basics/memory_defer.ae) -- defer pattern (recommended)
- [examples/basics/memory_manual.ae](../examples/basics/memory_manual.ae) -- manual free pattern
- [examples/basics/memory_escape.ae](../examples/basics/memory_escape.ae) -- returning allocated values
