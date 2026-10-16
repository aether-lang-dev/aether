# One ownership model

Status: approved by the owner on 2026-10-14, with the recommended answers below. Stage 1 is in progress; nothing else is implemented yet.

## Why this exists

In two weeks (2026-09-29 to 2026-10-13) 429 issues were opened against
Aether, 415 of them by our own probes and reviews. The share that is about
ownership (who frees a string, a closure environment or a config, judged by
issue title) grew from 9% (Sep 29 to Oct 6) to 26% (Oct 7 to 12) to 37%
(Oct 13), and 28 of the 36 ownership issues filed on Oct 13 are still open. Each fix is correct for the
shape it covers, and the review of each fix finds the next shape: #2868 (an
`fn[N]` local never released its closures) led to #2876 (the same in a loop
body), which led to #2878 (an element handed back, the array captured) and
#2879 (the array in a cell). The list does not end because the model is a
list.

This document proposes replacing that list with one rule, applied the same
way to every kind of value in every place it can live.

## The model today

[memory-management.md](../memory-management.md) describes about fifteen
rules, each a section of its own. In short:

- **A `string` has several physical shapes.** A literal is static `char`
  data. A fresh string from the runtime (`string_concat`, interpolation,
  `string.from_int`, ...) is a refcounted `AetherString`, recognised by a
  magic header. A literal that holds a NUL is a static `AetherString` pinned
  so it is never counted (#2520). A `char*` from C is whatever C says.
  `string_retain` and `string_release` do nothing to a value without the
  header, so the runtime cannot tell an owned plain buffer from a borrowed
  one.
- **Ownership is tracked per variable, not per value.** Each string local
  has a `_heap_<name>` flag the compiler sets on every assignment from what
  it can prove about the right-hand side. Every other place a string can
  live (a struct field, an array element, a tuple slot, an optional, a
  module variable, a closure cell, a container, a builder config, a message)
  needed its own flag or its own convention, and each was added by an issue.
- **Transfers are decided by reading the callee's body.** Whether a caller
  frees what it passed depends on whether the callee keeps, frees or
  returns the parameter (copy-on-keep, `@retain`, `@noescape`, the
  hand-back rules); whether a caller frees what it got back depends on a
  classification of every `return` in the callee (the uniform-heap wrap, the
  pass-through veto, the per-position tuple classifier, the cycle rules of
  #2870 and #2875). These analyses are whole-program, can loop through
  recursion, and stop at a binary import (#2860, #2861).
- **Closure environments are already counted** (#2494), with atomic counts,
  and a holder takes a reference of its own (#2525, #2528). Structs that own
  strings or closures have generated `_destroy`, `_dup` and `_replace`
  functions. These are the parts of today's model that converged.

The pattern behind the open issues: wherever the compiler cannot see which
shape a value has, or which body a call reaches, it guesses, and each wrong
guess in a new place is a new issue. The parts that converged are the ones
where the value carries its own count.

## The proposal

Five rules. Each is a statement about every value kind and every place, with
no exceptions listed per shape.

### R1. Every owning value carries its own count

- Every `string` the program holds is an `AetherString`: the runtime's
  producers already return one, a literal is emitted as a static pinned
  `AetherString` (as #2520 already does for literals holding a NUL), and a
  `char*` that comes in from C is adopted or copied once, at the extern
  boundary (R4).
- Closure environments keep their count (they have one today).
- A struct, tuple, optional or fixed-size array that holds owning values is
  not counted itself. It is a composite whose copy, destroy and move are its
  parts' copy, destroy and move, generated per type (the `=copy`, `=destroy`
  and `=sink` hooks of #484, which the generated `_dup` and `_destroy` of
  structs already are in all but name).

Consequence: `release` is always correct on any string value, and the
`_heap_<name>` flags, the "is this expression heap" classifier and the
uniform-heap return wrap have nothing left to decide.

### R2. Every place holds exactly one reference to what it holds

A place is anything a value can be stored in: a local, a parameter the
callee stores, a struct or message field, an array or slice element, a tuple
slot, an optional's payload, a module variable, a closure cell or captured
variable, a container slot, a builder config, an actor's state.

### R3. Three operations, the same everywhere

- **Store** into a place: take a reference to the new value, then release
  the value the place held. A temporary that dies at the store is moved
  instead (no count change). This one rule replaces the per-place "adopt,
  move, copy or borrow" decisions.
- **Leave** a place (scope exit on every path, `break`, `continue`,
  `return`, a caught panic; a struct, array or container destroyed): release
  what it holds.
- **Pass** a value to a parameter: the callee borrows it for the call. If
  the callee stores it, that store is R3's store and takes its own
  reference. The caller never needs to know what the callee does with it.
- **Return**: the callee hands the caller one reference. A value it only
  borrowed (a parameter, a global, a field of a parameter) is retained at
  the `return`.

No rule depends on reading another function's body, so no rule changes at a
binary import, a recursive cycle or a clause set.

### R4. C boundaries are the only place that needs annotations

An extern returning `char*` is borrowed unless declared `@heap`; a borrowed
one is copied into an `AetherString` when it is stored, an `@heap` one is
adopted once. A `string` passed to C is passed as its character data. This
is today's `@heap`, `@borrow`, `@retain` and `@noescape` vocabulary, kept
for C and no longer needed for Aether functions.

### R5. Optimisations may remove counting, never decide ownership

Moving on last use, eliding a retain and release pair, and borrowing instead
of retaining a parameter that is only read are optimisations over R1 to R4.
When one is wrong or missing, the program does one count operation too many;
it does not leak or free twice. Today the same analyses decide correctness,
which is why a gap in one is a leak or a use after free.

## What it settles

Decisions that are open today, each an instance of R2 and R3:

- **#2871, #2860, #2861 (a builder's config):** the config is a place the
  builder call owns. A setter that keeps `_ctx` stores it (R3), so it holds
  its own reference; the call releases its reference when it returns. A
  factory that returns a shared object returns a retained reference (R3's
  return). No rule about what a builder body names.
- **#2747 (a store through a `string[]` slice element):** a slice is a view;
  storing through it stores into the element of the array it views (R3).
- **#2822 (an optional field that owns memory):** an optional's payload is a
  place (R2); copy and destroy are its payload's.
- **#2808 (module-level strings never given back):** a module variable is a
  place; a re-bind is a store.
- **#2869, #2836, #2844 (tuples):** a tuple slot is a place; a tuple local
  owns its slots; a thrown-away tuple is left at once.
- **#2878, #2879 (closure arrays):** an element is a place holding a
  reference to an environment, as a closure local does today.

The other open ownership issues (#2786, #2788, #2814, #2825, #2828, #2849,
#2852, #2858, #2872, #2875) are shapes where today's analyses guess wrong.
Under R1 to R3 there is nothing to guess. Each becomes a cell of the test
grid below and is closed by the stage that makes its cell pass, not by a
fix of its own.

## What it costs, and the risks

- **Count traffic.** Every store of a string that is not a dying temporary
  becomes a retain. Parameters stay free (R3 borrows), and R5's moves remove
  most stores of fresh values. Interpolation moving to the refcounted shape
  cost +1.6% on a loop that does nothing else (memory-management.md). The
  benchmark suite gates each stage; a stage that costs more than its
  measured budget does not merge.
- **Threads.** `AetherString` counts are plain `int`s, while closure
  environments use atomic counts. A string must not be shared by count
  between actors. Proposal: a send copies a string unless its count is 1,
  in which case it moves. This needs checking against what the send side
  does today before stage 2.

  Decided 2026-10-15 (#2919): a place other threads can read (a module
  `var`, a closure environment) can make two actors keep the same string,
  and R3 keeps by retaining where today copies. A string stored into such
  a place is published: a bit of its `ref_count` marks it shared, and its
  retain and release are atomic from then on. Every other string keeps its
  plain count. The value carries the state, as it carries its count.
- **Cycles.** A closure environment that ends up holding itself (a closure
  stored in a cell it captures) is never freed by counting. That is today's
  limit too; the model does not make it worse, and a cycle check is out of
  scope here.
- **C code that holds an Aether string past a call** (a C library keeping a
  `const char*`) must copy it or be declared `@retain`, as today.

## How to get there

Each stage ships on its own, keeps the full test suite and the leaks gates
green, and is measured against the benchmarks before it merges.

1. **The grid first.** A generator writes one test program per cell of
   value kind (string, string?, closure, `fn[N]`, struct owning strings,
   tuple with strings, builder config) by place (R2's list) by transfer
   (store, re-store, leave on each exit path, pass, keep, return, hand
   back, send, catch a panic). Each program checks its values, checks
   `mem.steady_growth` is 0, and runs under the poisoning allocator. Run on
   main, it is the map of what is broken today; from then on it replaces
   probing by hand and filing one issue per shape.
2. **R1:** every string an `AetherString`; literals pinned; C strings adopted
   or copied at the boundary.
3. **R2 and R3:** one generated take and drop per type, used by every
   place; the per-place paths go.
4. **R3's transfers:** parameters borrow, the callee's own store retains,
   returns hand over. The callee-body analyses go: copy-on-keep, the return
   classifier, the pass-through veto, the tuple and cycle classifiers.

   A function may not free a value it borrows (a parameter): the checker
   refuses `string.free` or `release` of a parameter, with a message
   pointing to keeping the value (store it) instead. Under R3 such a free
   would release a reference the function does not own. The grid's cells
   for a function that frees what it is passed, on every path (T8) or on
   some (T8S), have REFUSED as their target.
5. **R5:** last-use moves and pair elision, each measured.
6. **Docs:** memory-management.md rewritten around R1 to R5; the per-shape
   sections become history.

## Implementation record

What has landed, and the decisions taken while implementing, so later stages
build on them instead of re-deciding.

**R1 (#2913, #2914, #2915, inventory in `ownership-r1-inventory.md`).** Every
literal is a static pinned `AetherString`. The runtime's string producers
return counted strings. R4 as built: an extern's `string` or `ptr` parameter
gets the characters, unless the parameter is declared `@aether ptr` or the
function is string-aware (`string_*`, `aether_string_*`); a C variadic `...`
gets the characters; a store through a C `string[]` view writes the
characters; the std containers declare `@aether ptr` so they keep the header.
An extern `-> string` is C text, ended by its NUL. Bytes whose length C
reports separately (a digest, compressed data, a read buffer) cross as `ptr`
and become a string through `string_new_with_length(p, n)`: taken as a
string, they would be copied up to their first zero byte.

**Published strings (#2919).** Bit `0x40000000` of `ref_count` marks a string
shared; its retain and release are atomic from then on, every other count is
plain. `aether_string_publish` sets it and leaves pinned strings alone.
Published where it is stored: a module variable, a closure's captured string
or shared cell, a container element (list, map, string sequence, strarr:
`string_retain_shared`), and a struct field reached through a pointer. A
message does not share: a send deep-copies its strings (#466), so it needs no
publish. A value read from a published place carries the bit with it.

**Within the budgets (#2925).** Measured on an Apple M1 Pro, each
`benchmarks/ownership/` program built by #2916's merge (c5f20025) and by its
first parent, the same output from both, 21 runs each interleaved, the
fastest of each compared. R5 as built: a literal is pinned, so taking one is
the literal itself (no run-time probe), a list adds it as an element it does
not own, and the pinned strings are `const`, so the C compiler folds a probe
of one away. After that, against the base:

| benchmark | change |
|---|---|
| alias | -87.5% |
| fields | -51.9% |
| closure | -38.7% |
| interp | -14.5% |
| concat | -1.4% |
| splitjoin | -0.5% |
| listheap | +0.8% |
| list (a literal) | +3.1% (+44.3% before) |
| seq (cons) | +13.0% |
| passret | +23.2% |

Two are accepted as what the model costs, not tuned further:

- **passret,** a function returning its parameter: the return hands over a
  reference (R3), a retain in the callee and a release in the caller, about
  0.8 ns a call (3.46 to 4.26 ns). The base returned a borrowed pointer,
  the shape behind the use-after-free bugs #2916 fixed. Removing the pair
  needs to know the callee returns its parameter, which is reading its
  body.
- **seq,** a cons: a sequence crosses actors by reference, so its cells'
  counts are atomic, and the base raced on them. Plain counts until a cell
  is published, as for strings, measured 2 to 5 points of the 13, and a
  place that failed to publish would be a data race; the atomics stay.

The concurrent cache benchmark (#2935), against the same base, 31 runs each
interleaved, compared by median ops/sec with a 95% bootstrap interval: every
row's median is within the 3% budget, the worst -2.4%. The COW snapshot cell
rows lose 1.6% to 2.3% (intervals within -1.3% and -2.5%), the in-flight
count a send from the main thread takes (#2912). Of the actor-backed rows,
the write-heavy single-owner actor is 46.7% faster, the read-heavy one -2.2%
[-3.3%, -1.2%], and the write-heavy sharded actor map -2.4%, an interval
([-14.2%, +8.7%]) its run-to-run spread leaves too wide to resolve.

**Returns (#2920).** An Aether function hands its caller one reference from
every return: every `-> string`, and every `string` and `string?` position of
a tuple. A pinned literal is handed over as it is; a borrowed string is
copied until retain-on-take lands. A tuple returned whole (`return g()`,
`return t`) copies each position its source did not hand over (a C function's,
a closure the compiler cannot name). The caller always moves the result into
its place.

**C strings in (#2920).** An `@heap` extern's plain `char*` is adopted into an
`AetherString` at the call (`aether_string_adopt`), so every string the
program holds is counted and `release` never frees a plain pointer.

**Locals (#2920, R2).** Every string local owns what it holds. A store moves a
fresh value, or an owning local at its last use; anything else is taken.
Taking retains a counted string (strings are immutable once made; nothing
writes into one after it is shared), hands a pinned one over as it is, and
copies only a plain C string. Every other place is a place like a local: a
module variable, a struct field (by any path, and in a struct literal), an
actor's state field, a container element (`list.add`, `list.set`, `map.put` of
any `string`: the container adopts the taken reference and releases it with
the element). The store takes the new value and releases the one it replaces;
a module variable's last value lives as long as the program. A fresh string
passed to a `string` parameter is released after the call. A `ptr` slot is
not a string place: a string stored there is C's (R4).
`string.free(x)` or `release(x)` on a local releases its reference and leaves
the local empty; reading it afterwards reads nothing, as reading freed memory
would in C, and a pinned literal it held is untouched.

**Escape channels, measured.** The escape walk kept a local alive (no free on
rebind or exit) when its value might be kept elsewhere. With each channel
switched off in turn over the whole suite: a store into a closure's captured
variable breaks nothing (such a variable is a shared cell, which takes); a
call argument breaks only a string handed to an Aether function's `ptr`
parameter that stores it (a container, #2922); a store into a field or an
element breaks views: an element of a `string[]` slice over memory C owns
(std.strarr, a sort swapping through `tmp`) is no place that owns, and a copy
taken from it and stored back dangled when the copy was freed. Retain-on-take
removes that: the local and the view hold the same counted string. A
header-defined struct's string field (`@c_import`) is C's, as R4 says; it kept
the value alive by the escape rule until the walk was deleted (see "C's
places are given a reference" below).

**A string kept through a `ptr` (decided 2026-10-15).** A `ptr` slot is
untyped memory and owns nothing, so a `string` that reaches one and is kept
there would live as long as its owner, not as long as the slot. The keep
analysis caught this by reading callee bodies; it is replaced by a rule in
the signature, so no call site reads another function's body:

- An Aether function's `ptr` parameter is borrowed for the call. One the
  function keeps (stores where it outlives the call, returns, captures, or
  passes to another function's keeping parameter) is declared `@keep`, as an
  extern's is declared `@retain` (R4).
- The checker verifies each function against its own body: a `ptr`
  parameter not declared `@keep` is not kept. A call is checked against the
  callee's signature.
- A `string` goes implicitly to a borrowed `ptr` parameter. To a `@keep` one,
  or into a `ptr` field (a store or a struct literal), it goes only through
  `string.keep(s)`, which hands over one reference the program gives back
  with `string.release_kept`; the implicit form is refused (E0902). A `ptr`
  module variable or local already refuses a string by type.
- A closure's `ptr` parameter is always borrowed, and a function with a
  `@keep` parameter is called by name: a call through a value cannot see
  what the callee keeps. A typed pointer (`*T`) and a `distinct` type over
  `ptr` are left alone: a string converts to neither.
- Measured cost: 329 `@keep` annotations across 60 files of std, contrib and
  the tests, applied until the checker found no more (keeping cascades up
  through every function that passes its own `ptr` on to a keeper).
- The std containers' `ptr` entry points take strings through the string
  routing (a string given to `list.add` is taken by the list), not as a
  kept `ptr`.

**A borrowed argument outlives its call.** The callee borrows, so the
caller guarantees the argument lives through the call. A local or temporary
of the caller does: the callee cannot reach it. A string read from a place
the callee can reach and rewrite (a module variable, a field or an element
by any path) does not: a store into that place inside the call released what
the argument pointed at. The caller holds such an argument for the call (a
reference of its own, released after it), as it already held a closure
cell's value (#2755). A call into C is left alone; C does not rewrite Aether
places.

**Counting is inline.** A take and a release of a string no other thread can
reach are a few instructions in the generated code (the header layout comes
from the runtime's own definition); a shared string, and a release that
frees, call into the runtime. Against out-of-line calls this cut a loop of
5,000,000 borrowed local stores from 61 ms to 31 ms, and one of field stores
from 34 ms to 20 ms (the R1 compiler, which counted none of them: 9 ms each).

**The unwind journal knows places (#2923).** A panic runs the releases a
longjmp skips. The journal used to hold values: each store tracked the value
and each release forgot the newest entry with that pointer. Once two places
can hold the same counted string, a release forgot the other place's entry,
so a panic skipped a release (a leak) or ran one twice. Now a place that owns
(a string local or temporary, a struct local) is journaled by its address
where it is declared or first set, with the address of its ownership flag;
a drain releases what the place holds then and empties it. Stores, moves and
releases never touch the journal; a scope exit forgets the place. The drain
takes its entries off the journal before it runs any release.

**Temporaries live to the end of their statement when a view may point into
them.** `string.bytes(s)` returns a view of `s`; given a temporary
(`string.bytes(make())`), freeing the temporary as soon as the call returned
left the view dangling. As in C++, such a temporary (a fresh string passed to
a call whose result is a slice or a `ptr`) lives until its statement is done:
its block releases it on every exit, and a statement run again releases the
one before. Other temporaries are still released right after their call.

**An operand read ahead of a call takes its own reference.** A field read
evaluated before a later operand that may store into the field (#2798) holds
a reference of its own until its statement is done, so the store cannot free
what the call is given.

**Order of the remaining #2920 steps.** (1) Switch off each escape channel
whose receiver takes (done 2026-10-15: the escape walk is deleted). (2) The `_heap_` flags are then always 1 and are
deleted, with the heap classifiers that set them, copy-on-keep and the callee
keep analysis. (3) `--diagnose=ownership` reports R3 (moved or taken, per
store), rewritten once, after (1).

**A return is a store into the caller's place.** A string return takes its
value as any store does: a local on its last use moves out, anything else is
retained. The return-escape channel (the set of locals a `return` names,
their skipped exit free, the free each other `return` ran ahead of the
function's defers) is gone, and with it a free that ran before a user `defer`
reading the local. Structs, tuples and optionals return the same way; the
struct return-escape set went with it.

**One retain and one drop per type (#2921).** Every owning type has two
generated functions, used by every place: `_ae_retain_<T>(T*)` takes a
reference to each part of a bit copy, `_ae_drop_<T>(T*)` releases each part
and empties it; `_ae_publish_<T>` makes a value's strings atomic for a place
other threads read. A struct's are `_aether_copy_<Name>` and `_aether_drop_<Name>`; a
tuple's, an optional's and a fixed-size array's are emitted by
codegen_own.c once every type is complete, prototypes first, so nested
composites call each other in any order. A part is a string, a closure's
environment, or a composite of those.

Struct fields lost their `_heap_<field>` trackers: every string field owns
what it holds, so a struct's C layout is its declared fields (the punning
idiom of #1879 holds by construction) and `reflect.field_heap_offset` is
gone. `_aether_store_<Name>` is a store (keep the old value, put the new one in,
destroy the old), a move empties the moved-from parts. A struct parameter
borrows its caller's value; one the body writes into is copied on entry and
destroyed at its exit, which is a question about the function's own body.
Copy-on-keep for structs (the "kept" walk, the disown and capture on entry)
is deleted.

Tuple, optional and array locals, struct and tuple fields, module `var`s,
array and slice elements, message fields and `_ = v` discards each take and
drop through these functions. A struct definition is emitted after every type
it holds by value, tuples and optionals included.

**Array results (#2894).** A named function returns a fixed-size array,
alone or at a position of a tuple, in a struct of its own (C has no array
results). A binding moves the elements out, a struct field or a tuple
position takes them, and a call used as an array is held in a statement
temporary that its block drops. A closure, an extern and a function type
still return no array (E0201).

**Owning optional fields (#2822).** An optional field whose value owns
memory is a part like any other: a literal or a store takes, the struct's
destroy and a store release.

**Every call borrows its arguments (decided 2026-10-15).** A fresh value
passed to any call (a function, a closure, a typed function pointer, an
extern) is released after the call, and journaled for it, since the callee
may panic. Nothing is read from the callee to decide it: a callee that keeps
or returns an argument takes a reference of its own (R3), and C that keeps
one copies it or declares the parameter `@retain` (R4). A closure the program
did not compile (a host's box, a `@c_callback`'s argument) borrows the same
way. The hand-back classifier, the callee memo, the closure-argument and
fn-pointer conventions and the escape walk, all of which read other
functions' bodies, are deleted.

**`@retain` hands over a reference (R4).** A call to an extern parameter
declared `@retain` hands it one reference: a fresh value as it is, a place's
retained. This holds for a `string` and for a closure; the std externs that
keep a closure (observe, worker jobs and the main-thread poster, the http and
net server handlers, the QuickJS function binding) are declared so, and they
release what they keep through the closure's own destructor.

**C's places are given a reference (R4).** A string stored into a
header-defined struct's field or into a slice over C memory is given a
reference of its own, which nothing in the program releases; the local it
came from keeps its own and releases it as any local does.

**A closure in a `ptr` slot (#2926, decided 2026-10-15).** A `ptr` slot owns
nothing, as for strings: a closure kept in one goes in a box holding a
reference of its own (`box_closure(f)`, or the coercion a store into a `ptr`
makes), and the program gives the box back with `release_closure(p)`, the
closure counterpart of `string.release_kept`. A `ptr` parameter that keeps
what it is given is declared `@keep` and is given that heap box; any other
`ptr` parameter only borrows, so it is given a box on the caller's stack and
nothing is allocated. Containers keep their own reference as before.

**A `defer` runs when a caught panic leaves its scope (#2923, decided
2026-10-15).** The language reference says a plain `defer` runs on every
exit; a panic skipped it. While its scope is live, a plain `defer` is in the
unwind journal with a thunk: the deferred statement, generated where it is
written, over copies of the frame's locals it names, copied back after it
runs. The journal drains before the jump, newest first, so a deferred
statement sees every local declared before it. A normal exit forgets the
entry and runs the statement in place. `defer try` and `defer catch` are not
journaled: a panic is neither outcome. A frame per `defer` (a setjmp at each
registration) was the alternative; it counted against the panic nesting
limit, so a recursive function with a `defer` inside a `try` would abort.

**No per-local flags (#2920, step 2).** A string local always owns what it
holds: every take hands it a reference (a fresh value moved, a local at its
last use moved out and left empty, anything else retained, a C string
copied), so `_heap_<name>` is gone, with the actor state's trackers. A store
takes the new value before it releases the old one, so `s = s` is a retain
and a release; a scope exit releases unconditionally. A `catch` binding
takes its reason when the runtime did not hand it over. The catcher owns
the reason whatever its binding is: `catch _`, and a binding the handler
reaches through a closure environment, a capture cell or actor state, read
it from a hidden owner of the handler's scope that releases it like any
string local.

**Returns are typed, not classified.** Whether a call hands its caller a
string is read from the callee's declared result: an Aether function
returning `string` (or a `string` tuple position) always does, and an
extern does when it says `@heap`, on a `string` or on a `ptr` result (a
counted string returned untyped, `string.from_int`). The return classifiers
that walked function bodies, their cycle breaks and memo, and the producer
name list are deleted.

**No body-reading analysis is left.** The parameter escape walk, the
closure environment scan (EnvScan), the hand-back walks and the copy-on-keep
query are deleted. A closure cell always owns the closure it holds: a call
lends it with a reference held for the call, and whatever keeps or returns
it takes its own (#2788). A function frees a caller's string only when it is
a consuming free like `string.free`, since an Aether function may not free a
parameter it borrows (E0901).

**String sequences own like every other kind (#2921).** `*StringSeq` is an
owning type of the generated take and drop: a place takes a fresh sequence
(a literal, an Aether function's or a closure's result, an extern's whose
result says `@heap`) and retains any other, and drops what it holds when it
is overwritten or left; a struct field, a tuple position, an optional, an
array element, a capture, a cell, a message field, actor state and a module
`var` follow from that. The std producers say `@heap`; `string.seq_tail`
lends. The per-local `_seqheap_` flags, the escape walk that kept a
sequence alive when it was passed to anything but a `string.seq_*` op, and
the producer name list are deleted. A sequence crosses threads in a message
or a capture, so its cell counts are atomic, as a closure environment's
are; the builders adopt the tail they are handed instead of retaining and
releasing it, so a build costs no more than before. A send takes a
reference rather than copying the cells, which are immutable.

`string.seq_free` of a place (a local, a field, an element) releases what
the place holds and leaves it empty, so the place's own release (its scope
exit, its box's `heap.free`) finds nothing; std.language freed a field by
hand and then the box, which released it twice.

**Containers and counted strings typed `ptr`.** A string a `-> ptr @heap`
constructor returns (`string.from_int`) is a counted string handed to the
caller, and `list.add`, `list.set` and `map.put` adopt it as they adopt a
`string`; routed to the plain add, the list kept a pointer the call's
argument temporary then released. A container handed a pinned string (a
literal) stores it as an element it does not own: a pinned string is never
released, so owning it would only cost the container its ownership tags.

**A borrowed binding that is rebound.** A parameter, a list pattern's head
and tail and a receive arm's field binding borrow their value. One of an owning
tuple, optional, closure or sequence type that its body rebinds takes a
reference of its own first, which its scope exit drops, so the rebind
releases nothing of the owner's (a struct parameter already did this,
#2582). The checker refuses `string.seq_free` of a borrowed sequence
parameter, as it refuses freeing a string one (E0901).

**C memory owns nothing.** A string stored into C memory viewed as
`string[]` (`(names as string[])[i] = s`) gives C its characters and takes
no reference: the program keeps the string alive while C reads it, as for
any `ptr` slot. The checker refuses a string built in the statement itself
(a call, an interpolation), which nothing would keep (E0902); bind it to a
local first. An earlier pass gave the stored string a reference of its own,
which nothing could release (C holds only the characters), so every such
store leaked.

**A literal may be kept as it is.** E0902 refuses a `string` given to a
`@keep ptr` without `string.keep`, but a string literal is pinned and never
released, so a slot may keep it as it stands (`pqueue.push(q, 1, "job")`).

**A permutation takes nothing.** Sorting moves the elements' pointers
(`mem.swap_ptrs`) and compares through borrowed reads, so it neither copies
nor releases: the array may be Aether's or C's (a `strarr` view).

**A program re-declaring a std extern declares its contract.** An extern
says `@heap` when C hands over the string it returns; a program that
declares `string_concat` itself without it is telling the compiler the
result is borrowed, and the copy then leaks the original. The regression
programs that re-declared std functions say `@heap` as std does.

**Teardown in constant stack.** A closure's environment releases what it
holds when its last reference goes, and what it holds can be a closure
whose environment then goes too: a chain of rebinds to a closure calling
the one it replaced (grid cell K3-P2-T13) tore down one C frame per link,
and overflowed Windows' 1 MB stack at 50,000. An environment whose count
reaches zero hands itself to `aether_teardown`, which runs it at once
unless a teardown is already running on the thread, and then queues it for
that one to run after it returns. The queue is the thread's, grows only as
wide as one teardown's fan-out, and is freed with the unwind journal at
thread exit.

**A builder's unused config (#2871).** A builder owns the config its call
hands it (it frees it, or returns it as what it built). One whose body never
names `_builder` does neither, so its exit releases the config through its
factory's release, decided from the builder's own body.

**A builder across a binary import (#2860).** A library built the default
way links its own copy of the runtime, so it cannot free a config the
program's copy made. Its catalog (schema 1.8) marks each builder that never
names its config; the stub `ae` writes for a binary import passes such a
builder no config and never names `_builder` itself, so the program
releases its config on its own runtime, as for any builder that ignores
it. What an export returns crosses the same boundary and is #2931.

**Fixed-size arrays in every place (#2921).** A message field holds an array
by value and owns its elements (filled once the message exists; a receive arm
binds a view of it); an optional wraps one through a temporary its array is
filled into; an array literal of owning elements passed to a function is a
statement temporary, journaled and dropped by its block; one discarded is
filled into a temporary and dropped. A store into an element of an owning
array local takes and drops whatever the element holds.

**A string an export returns (#2931).** R3 at the C boundary: an
`--emit=lib` export's `-> string` (and a builder's) hands its caller one
reference, as the counted string's handle (`const void*`), and the caller
gives it back through `aether_<lib>_string_release`, which the library
exports and its catalog names (schema 1.9). This replaces recommendation 1's
"a `const char*` to the characters at the boundary", which nothing could
release: characters cannot be mapped back to their string (an adopted
buffer is not stored after its header), and a per-thread "last result"
freed by the next call would be a deferred-free list. A copy the caller
frees costs a copy per call and loses bytes after a NUL without a length;
the handle costs nothing and keeps every byte. `<lib>` is the package, else
the entry file's stem, so every target built from one source exports the
same release (a `--namespace` library is built from a generated file, so
`ae` names it). A binary import on the shared runtime takes the handle
(`-> string @heap`); with a runtime of its own, its stub copies the string
in (`aether_string_import`) and gives the library's back.

**Two copies of the runtime (#2931, #2861).** Each copy frees only what it
made. A string crosses as a copy, since its bytes are all there is to it. A
struct holding a string has no such copy: by value, the program's drop
releases the library's strings, and through a typed pointer the program's
stores do. A builder that uses its config fills, grows or frees a map the
program made, and a factory in the library would not help, since the
trailing block fills the config with the program's runtime. So where the
copies stay apart (macOS, Windows: no ELF interposition) `ae` refuses to
import a static-runtime library whose exports pass such a struct or whose
builders use their config, and names the rebuild with `--shared-runtime`.
Closures cross either way: an environment carries its own release, which
runs in the library that made it.

## Decisions needed

Stage 1 (the grid) needs none of these: it only measures, and it starts
now. Each decision has a recommended answer; approving the document
approves the recommendations.

1. **R1: is every Aether `string` an `AetherString` (pinned for literals)?**
   Recommended: yes. `string_concat`, interpolation and the `from_*`
   functions already return one and #2520
   already pins literals holding a NUL, so this finishes a move that is
   half made. What C sees through `--emit=lib` and bindgen stays a
   `const char*` to the characters at the boundary (R4); stage 2 checks
   every exported signature before it merges.
2. **R3: parameters borrow, and a callee that stores takes its own
   reference, for every Aether function.** Recommended: yes. It is the
   rule that removes reading other functions' bodies, which is where the
   reviews of #2858, #2869, #2875, #2876 and #2878 found their
   regressions. The C-only annotations of R4 stay for externs.
3. **Strings in sends: copy unless uniquely held, or atomic counts?**
   Recommended: copy unless the count is 1, in which case move. Actors do
   not share memory by design; a copy at the send keeps every other count
   operation a plain increment, where atomic counts would make all of them
   slower to serve the one place strings cross threads. Closure
   environments keep their atomic counts as they are.
4. **The order: no new per-shape ownership fixes.** Recommended: yes, in
   force since 2026-10-13. Ownership findings go into the grid, and the
   stages close them.
