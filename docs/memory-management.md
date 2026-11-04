# Memory Management

Aether has no garbage collector. Every allocation is released at a point the
source shows, in one of three ways:

- **Values the program owns are counted by the compiler.** Strings, closures,
  string sequences, and the structs, tuples, optionals, sums and fixed-size
  arrays built from them carry a reference count. Every place that holds one owns a
  reference, a store takes one, and leaving a scope releases what the scope
  held. The program writes no free for them.
- **A slice's buffer is counted.** The buffer `make([]T, n)` hands out, or
  `append` and `push` grow, is held by every place that holds a slice of it,
  a view included, and is released with the last ([Slices](#slices)).
- **Handles are freed by the program.** A container (`list.new()`,
  `map.new()`), a C buffer (`malloc`), a parsed document or an open file is a
  `ptr`, and a `ptr` owns nothing by itself. The module that made the handle
  has a `free` or `close` for it, and `defer` keeps that call next to the
  allocation.

Counting is deterministic: a program releases the same values at the same
points on every run, and nothing pauses to collect. Its one limit is a cycle:
a value that reaches itself through its own references is not released
([Cycles](#cycles)). The rules below are checked by a generated test grid that
CI runs on every pull request ([How the rules are tested](#how-the-rules-are-tested)).

## Values the program owns

Five rules decide where a counted value is released (the design and its
implementation record are in [design/ownership-model.md](design/ownership-model.md)):

- **R1, every value carries its own count.** A `string` is a counted
  `AetherString`; a literal is pinned (its count never changes and it is
  never freed). A closure's environment is counted. A composite owns each of
  its parts.
- **R2, every place holds one reference.** A local, a parameter's copy, a
  struct, tuple or optional field, an array element, a module `var`, an
  actor's state field, a message field, a list or map element, a closure's
  captured variable: each owns what it holds.
- **R3, a store takes and a scope exit releases.** A store takes the new
  value and releases the one it replaces; leaving a scope releases what its
  locals hold, on every exit (a `return`, a `break`, a caught panic). A
  parameter is borrowed: the callee never releases it, and one it keeps it
  takes a reference to. A call borrows its arguments, so a fresh argument
  is released after the call, an `if` or a `match` that gives a fresh value
  in a branch among them; a function returning a value hands its caller
  one reference.
- **R4, only C boundaries are annotated** (below).
- **R5, moves.** Taking a fresh value (a call's result, an interpolation)
  moves it; so does taking a local at its last use, which leaves the local
  empty. Otherwise a take retains a counted string, passes a pinned literal
  as it is, and copies a plain C string.

No rule reads another function's body: what a call does with its
arguments, and what it hands back, is decided by the callee's signature.

```aether,run
import std.string

label(n: int) -> string {
    s = "item-${n}"                 // owns the interpolation
    return s                        // moves it to the caller
}

main() {
    a = label(1)                    // a owns the result
    b = a                           // b takes a reference; a still owns its own
    a = string.concat(b, "!")       // releases the old value, owns the new one
    println("${a} ${b}")
}                                   // a and b are released here
```

```output
item-1! item-1
```

### Strings

A string local owns the value it holds, whatever it was given: a literal, a
call's result, another local, a field read. `string.free(s)` or
`release(s)` on a local releases its reference early and leaves the local
empty; on a parameter it is refused (E0901), since the function does not own
it. Strings are immutable once made, so two places sharing one counted
string is safe; a string stored where other threads can read it (a module
`var`, a closure environment, any part of a value reached through a
pointer, a slice or another actor's reference: a field, an element, a whole
array, an optional, at any depth) is published, and its count becomes
atomic from then on.

### Reading a string out of a container or a `ptr`

A container element and a `ptr` are untyped, so a `string` local given one
reads it as text whatever it points at. The typed accessors say the value is
a string and check that it is one: `list.get_string(l, i)`,
`map.get_string(m, key)` and `string.from_ptr(p)` each return
`(string, string)`, the string with `""`, or `null` with `"not a string"` when
the element is some other pointer. The string handed back holds a reference
of its own, so it outlives the container.

```aether,run
import std.list

main() {
    names = list.new()
    defer list.free(names)
    list.add(names, "ada")
    nested = list.new()
    defer list.free(nested)
    list.add(nested, names)

    first, err = list.get_string(names, 0)
    println("${first} [${err}]")
    text, err2 = list.get_string(nested, 0)
    println("${text == null} [${err2}]")
}
```

```output
ada []
true [not a string]
```

A `string` local re-bound to a bare `ptr` (`name, _ = map.get(m, "name")`)
is error E0202, which names the accessor to use
([E0202](language-reference.md#a-pointer-given-to-a-string-local-e0202); it was
the warning W1005 in 0.830.0 and 0.831.0).

### Closures

A closure literal's environment is counted. A local holding a closure owns a
reference; capturing a variable takes one; a closure field, element or state
field takes one; a closure passed to an `fn` parameter is borrowed. A
variable a closure writes lives in a shared cell, counted the same way, so
the closure and its creator see one value. [closures-and-lifetimes.md](closures-and-lifetimes.md)
goes through each place a closure can be held.

### String sequences

A `*StringSeq` is counted per cell and owned like a closure: every place
holding one owns a reference, a fresh sequence (a literal, an Aether
function's result, a `string.seq_*` producer's) moves in, any other is
retained, and `string.seq_tail` lends. The counts are atomic, so a sequence
crosses actors by reference. See [sequences.md](sequences.md).

### Structs, tuples, optionals, sums and fixed-size arrays

Each owning type has one generated retain and one drop, which every place
uses: a copy retains every part, a drop releases every part. A sum's parts
are its variants: a sum owns when one of its variant structs does, and its
retain and drop are that variant's, for the variant its tag names. A struct
parameter is borrowed; one its body writes into is copied on entry. A
struct boxed with `heap.new(T)` owns its fields as a local struct does, and
`heap.free(p)` releases them before it frees the box. Out of memory,
`heap.new`, like every allocation the compiler makes for the program (a
captured variable's cell, `ref`, `lazy`, a closure's environment or box),
is a panic naming what ran out, and inside a library call it ends the call
with `AETHER_OUT_OF_MEMORY` (#3377); a `malloc` the program calls itself
returns NULL. A
fixed-size array is held by value wherever it goes: a local, a field, a
message field, an optional's payload, a function's result (returned in a
struct of its own), a temporary passed as an argument.

### Containers, module variables, actor state, messages

`list.add`, `list.set` and `map.put` take a reference to a string or a
closure, released when the element leaves (`list.remove`, `list.clear`, a
store over it, `map.remove`, `free`); a `list.set` that fails gives back what
it was handed. A module `var` and an actor's state field take what is stored
and release what they replace; the actor's state is released when the actor
is destroyed. So does a store into another actor's state through its
reference (`w.label = s`, `w.items[i] = v`, a part of one such as
`w.items[i].name = s`): the actor's state holds a reference of its own, and
since the actor reads it on its own thread, a string it takes is published,
as one stored through a pointer is. A message owns its fields: the send takes them, a receive arm
reads them in place, and the message is released after the arm. What the
arm keeps (in state, in a container) it takes.

### C boundaries (R4)

C sees a string as its characters. An extern borrows what it is given for
the call. An extern that keeps a value declares the parameter `@retain`: the
call then hands it a reference of its own (a fresh value as it is, a place's
retained), for a `string` and for a closure. `@aether` passes the counted
string itself, for C written against Aether's runtime. `@noescape` states
what a parameter that is not `@retain` already is.

An extern returning a fresh string declares it `@heap` (`-> string @heap`, as
`string.from_int` does; `-> ptr @heap` for a counted string returned
untyped; per position of a tuple result, `-> (string @heap, int, string)`); the
string is adopted at the call, and the caller owns it. Any other string an
extern returns is C's and is copied when a place takes it.

A string stored into a header-defined struct's field (`extern struct ...
@c_import`) is given a reference of its own, recorded by the characters the
field holds; nothing releases it behind C's back, and the program gives it
back with `string.release_kept(s.field)` when C is done with it (#3567). A
field read back is the string it was given, so `old = s.field`, a store of
the next value and `string.release_kept(old)` give the old one back. One
stored into C memory viewed as `string[]` is given no reference (see `ptr`
slots, below).

### `ptr` slots

A `ptr` slot owns nothing. A string goes into one only through
`string.keep(s)`, which hands over a reference the program gives back with
`string.release_kept(p)`. A module-level `ptr` var takes a string as it
stands (`gp = s`, `gp = string.concat(a, b)`) and gives it that same
recorded reference (#3567): the string outlives the local or the statement
it came from, and `string.release_kept(gp)` gives it back before the next
store. Nothing gives it back on a re-bind: as with `keep`, that is the
program's to do. C memory viewed as `string[]` owns nothing either:
`(names as string[])[i] = s` gives C the characters and no reference, so
the string must outlive every read. A local does only while the memory
ends with the function (an argv array built, handed to C and freed in one
function). Memory that outlives it (reached through a parameter, a
module-level `var`, a field or a heap box, or whose pointer is returned or
stored) takes `(names as ptr[])[i] = string.keep(s)`, given back with
`string.release_kept` where the memory is freed; any other string stored
there is refused (E0902), as is a string built in the statement. A literal
is pinned and goes into either as it is.

A closure goes into a `ptr` slot boxed (`box_closure(f)`, or the coercion a
store into a `ptr` makes), the box holding a reference of its own, which
`release_closure(p)` gives back.

A function that keeps a `ptr` parameter (stores it where it outlives the
call, returns it, or passes it to another keeping parameter) declares it
`@keep`; keeping one that is not declared so is E0902. A struct local to the
function, or a closure that runs and is dropped inside the call, does not
outlive it: putting the parameter in one keeps nothing unless the struct or
the closure escapes itself. A `ptr` parameter that only borrows is given a
closure in a box on the caller's stack, so nothing is allocated.

A function that only hands a `ptr` parameter to its result (a constructor
that builds a recorder, an iterator or a view around it) declares it
`@into` instead: it may return it, alone or in a struct, a tuple, an
optional or a closure, and nothing else. The caller holds the result as it
holds a struct local that holds its argument: in a local, read, lent to
borrowing parameters, and E0902 only where the result escapes with a
borrowed parameter of the caller. So `@into` does not spread up the call
chain as `@keep` does; a function that returns such a result made from its
own parameter declares that one `@into` too. A string goes to an `@into ptr`
through `string.keep(s)`, and a closure goes in a heap box with its own
reference. The full rule, with what counts as escaping, is in the language
reference's [String ownership](language-reference.md#string-ownership).

### Panics

A panic caught by a `try` releases what the scopes it leaves hold, as their
exits would have: every owning local, temporary and argument is recorded in
an unwind journal while it is live, and every plain `defer` of those scopes
runs, newest first. `defer try` and `defer catch` do not run on a panic.
That includes an owned pointer (`type T = owned ptr drop f`) in a local or
bound from a message field by a handler (#3406): while a frame that can
unwind is live (a `try`, an actor step) it costs one journal entry, about
3 ns at most, and outside one a single depth check.

### Cycles

Counting does not free a cycle: a closure stored in a cell it captures, or
values that hold each other through `ptr` slots, live until the program
ends. Break the cycle (store `none`, release the slot) before dropping the
last outside reference.

### Slices

A slice (`T[]`) is a view of elements, `{ ptr, len, flags }`. The buffer
`make([]T, n)` hands out, and one `append(xs, v)` or `xs.push(v)` grows, is
counted (#3568): every place that holds a slice of it holds one reference,
a view as much as the slice `make` returned, and the last place to give its
reference back releases the buffer, elements first (a string, a closure or
a struct that holds one, stored or appended into the buffer, is given back
as `free` gives it back). A program writes no `free` for it.

- **A local releases what it holds** on every exit of its scope (the end of
  the block, a `return`, a `break` or `continue`, a caught panic). Binding
  the local again releases what it held. A view keeps the buffer it views:
  `v = xs[1..3]`, `last = first_two(xs)` in an outer scope, a view returned
  or stored hold the buffer after `xs` is gone or bound again.
- **A buffer moves.** `ys = xs`, `return xs`, a store of `xs` into a field,
  a module-level `var` or an element, `xs` in a struct, a tuple, an array or
  a message, and `xs` given to a `@keep` parameter each move the buffer out
  of `xs`, which is left empty (its scope exit then releases nothing).
  Using `xs` after that, before it is bound again, is a compile error
  (E0906), and so is using it after `free(xs)` or `append(xs, v)`, which
  consume it. A local a view of which may be held (`v = xs[..]`, `xs` given
  to a function) is copied instead, each place given a reference of its own,
  so it can be used again.
- **A parameter borrows.** A slice argument is lent for the call: the
  function never releases it, and `free`, `append` or `push` on the
  parameter is refused (E0901). A parameter declared `xs: @keep T[]` takes
  a reference instead, the caller's moved out of a local that owns or a copy
  of its own, and releases it as a local does. A function that keeps what
  it is given (stores it, returns it) takes a reference of its own.
- **A copy of a slice another place holds takes a reference of its own.**
  Bound out of a parameter, a field, an element, a module-level `var`, it is
  released with the place that holds the copy. A copy of the whole buffer
  can grow: the buffer being shared, `append` moves the copy to storage of
  its own, so the other holders keep what they held.
- **Every place releases its reference**: a field (a struct's destroy, a
  store into it), an element of a slice of slices (a store into `g[i]`, the
  outer slice's release), a tuple's slot, an optional, a sum, a fixed-size
  array, a closure that captured it, a cell a closure shares, actor state
  and a message. A struct allocated by hand must start zeroed (`calloc`,
  `heap.new`): a store into its slice field releases what the field held.
- **`free` releases a place's reference early** and leaves the place empty,
  so the scope exit releases nothing twice, and neither does `defer
  free(xs)`. `free` of a view a place holds releases that view's reference;
  the buffer stays for the others. `free` of a view no place holds
  (`free(xs[1..])`, a pattern's tail) and of a view of memory no count covers
  (an array viewed as a slice, a string's bytes) is a named panic.
- **Not counted**: a slice local a pattern binds (a list pattern's tail, a
  match arm's binding) views what it was bound from, which holds the
  reference; a slice of C memory (`p as T[]`, a `T[]` an extern returns) is
  not counted, and the program frees it as before.

The rules, with an example, are in the language reference
([Slice ownership](language-reference.md#slice-ownership)).

## Handles and `defer`

A handle is a `ptr` to memory a module or C allocated: a list, a map, a set,
a string builder, a parsed document, a `malloc` buffer, an open file. A `ptr`
owns nothing, so the program frees each handle itself. `defer` schedules the
free when the handle is made, and runs it when the scope ends, in LIFO order,
on every exit: a `return`, a `break`, or a panic a `try` catches further up.

```aether,run
import std.map

main() {
    m = map.new()
    defer map.free(m)

    map.put(m, "k", "v")
    v, _ = map.get_string(m, "k")
    println(v)
    // map.free(m) runs here (scope exit)
}
```

```output
v
```

### The stdlib convention

A module's constructor returns a handle and its destructor frees it. In Aether
they are called through the module (`list.new()`, `list.free(l)`); the C
functions underneath carry the module prefix (`list_new`, `list_free`).

| Module | Constructor | Destructor |
|--------|-------------|------------|
| `std.list` | `list.new()` | `list.free(l)` |
| `std.map` | `map.new()` | `map.free(m)` |
| `std.set` | `set.new()` | `set.free(s)` |
| `std.strbuilder` | `strbuilder.new(n)` | `strbuilder.finish(b)` (the built string) or `strbuilder.free(b)` |
| `std.dir` | `dir.list(path)` | `dir.list_free(l)` |

A function whose C name ends in `_new` or `_create` returns an allocated
object, and its module's `free` (or `close`) is the destructor.

A string needs none of this. `string.new(s)` exists for C-facing code: it
returns a counted copy typed `ptr`, which no place owns, so a program that
calls it gives the copy back with `string.release`. Code that works with
`string` values never needs it.

### Several handles

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

### Returning a handle

A function that allocates a handle and returns it gives it to the caller,
which frees it:

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

### Actor state

An actor `state` field initialized with a constructor outlives any single
message handler. Free it in the handler that shuts the actor down:

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
            v, _ = map.get_string(data, key)
            println(v)
        }
        Stop() -> {
            map.free(data)
        }
    }
}
```

The runtime frees the actor's own allocation (the actor struct and its inline
mailbox) and releases its counted state (strings, closures) when the actor is
destroyed. A handle held in `state` is not part of that; free it yourself, as
in the `Stop` handler above.

### Common mistakes

**A handle with no `defer`:**

```aether,fragment
m = map.new()
map.put(m, "k", "v")
// LEAK: m is never freed
```

Pair the allocation with its free:

```aether,fragment
m = map.new()
defer map.free(m)
```

**A `defer` placed before the allocation:** `defer` registers when it runs.
Place it right after the allocation it frees.

**Allocating inside a loop:** `defer` is block-scoped, so a `defer` inside a
loop body runs at the end of each iteration, not once at function exit. An
allocation made inside the loop is freed on every pass:

```aether,fragment
while i < n {
    item = list.new()
    defer list.free(item)   // runs at the end of THIS iteration
    // ... use item ...
    i = i + 1
}
```

### Arenas

An arena is a region in which many small allocations share one lifetime and
are freed together. `std.arena` provides one, and the JSON parser uses one for
a parsed document's nodes. An arena is opt-in; the actor path does not use
arenas. Where `defer` frees one handle at scope exit, an arena frees a whole
batch at once. [allocators.md](allocators.md) covers `std.alloc`, the
allocator handle the containers accept.

### Summary

| Situation | What the program does |
|-----------|----------|
| A string, a closure, or a struct, tuple or optional of them | Nothing: the compiler releases it |
| A string read out of a container or a `ptr` | `list.get_string`, `map.get_string` or `string.from_ptr` |
| A handle the function allocates | `defer type.free(x)` right after the allocation |
| A handle returned from a function | The caller defers the free |
| A handle sent to an actor in a `ptr` field | The receiver frees it; the sender does not |
| A handle in actor `state` | Free it in the handler that stops the actor |

## `@scoped` bindings, checked non-escape

A binding annotated `@scoped` (`@scoped buf = ...`, or with `let` or `var`)
states that its value must not outlive the block that introduced it, and the
type checker enforces it. This
is opt-in escape checking for one binding, not a borrow checker (Aether has
none):

```aether,run
import std.string

total(n: int) -> int {
    @scoped buf = string.concat("abc", "def")
    return string.length(buf) + n      // a value derived from buf leaves
}

main() {
    println(total(1))
}
```

```output
7
```

The type checker refuses every way the binding's value itself could outlive
the block: returning it, assigning or aliasing it into another binding or a
struct field, placing it in an array, struct or message literal, capturing it
in a closure or a quote (either can outlive the block), and giving it to a
parameter that keeps it, as the callee declares (an Aether function's
`@keep` one, an extern's `@retain` one): the item of a list or map store
(`list.add`, `list.set`, `map.put` and their raw forms), the head or the tail
of `string.seq_cons`. A parameter that copies what it is given keeps nothing:
`set.add` copies its item, and a map copies its key.

```aether,fails
import std.string

leak() -> string {
    @scoped buf = string.concat("abc", "def")
    return buf      // error: `@scoped` binding 'buf' escapes its block: it is returned
}

main() {
    println(leak())
}
```

Only a value derived from the binding (`string.length(buf)`,
`list.size(l)`, a slice's `v.len`) may leave, and the error's help names the one
for the binding's type. A function the value is passed to is governed by its own
signature; `@scoped` checks the escapes written in the binding's own scope.

## How the rules are tested

The ownership grid (`tests/ownership_grid/`) generates one program per meaningful
combination of a value kind (a fresh string, a `string?`, a closure that
captures a string, an `fn[2]`, a struct owning a string, a tuple, a string
array, ...), a place (a local, a struct field, an array element, a tuple slot,
an optional, a module `var`, a capture or cell, a list or map, actor state, a
message field, a `ptr` slot, ...) and a transfer (an overwrite, a `break`, an
early `return`, a caught panic, a keeper, a return, an alias, ...). Each
program checks every value it reads against the one it stored, measures heap
growth over rounds of 100 calls, and runs a second time under an allocator
that poisons freed memory and stops on a double free.

Every cell is at its target. The baseline (`tests/ownership_grid/baseline.txt`)
records 1909 cells that run clean, and 30 in which a function frees a
parameter it borrows, which the compiler refuses (E0901). The Windows CI
workflow runs the grid on every pull request, where the heap count is exact,
and fails when any cell is worse than its baseline: a leak, a wrong value, a
use after free, or a program that stopped compiling. See
[tests/ownership_grid/README.md](../tests/ownership_grid/README.md).

## History

Before this model, each value kind and place had its own mechanism: a
per-variable ownership flag for strings (`_heap_<name>`), an escape walk
that kept a value alive when it might be stored, a uniform-heap wrapper on
returns, a copy-on-keep analysis of callee bodies, and per-struct field
trackers. Each fixed one shape and its review found the next. They are
replaced by the rules above; the tracking issue (#2918) lists the issues
that recorded them, and the implementation record is in
[design/ownership-model.md](design/ownership-model.md).

## Examples

- [examples/01-language/17-ownership.ae](../examples/01-language/17-ownership.ae): what the compiler releases (strings, closures, the structs that hold them) and what the program frees (lists, `heap.new` boxes, `make` buffers), `@keep`, and `string.keep` for a slot that holds a bare pointer
- [examples/01-language/16-defer.ae](../examples/01-language/16-defer.ae): handles freed with `defer` on every way out of a scope
- [examples/01-language/12-collections.ae](../examples/01-language/12-collections.ae): lists, maps and sets, each freed by the program that made it
