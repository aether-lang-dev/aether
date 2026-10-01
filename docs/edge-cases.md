# Edge cases: what is settled, and what only happens to be true

Behaviours of the language and standard library that surprise people, or
that nobody has yet decided on. Each entry says what happens, where it is
written down if it is, and which of these it is:

| Status | Meaning |
|---|---|
| **Settled** | Decided and documented. Changing it is a language change. |
| **By implementation** | True today because of how the compiler or runtime happens to work. No decision is recorded. It may change without being called a breaking change — if you depend on it, say so in an issue so it can be settled one way or the other. |
| **Under review** | A decision is pending; the issue is linked. |

What does **not** go here: bugs. A wrong answer, a crash, a leak is a
tracker issue, not an edge case — file it. This document is for behaviour
that is correct-or-tolerated but puzzling, and for behaviour whose status
has not been chosen. Append-only in spirit: when an entry moves from *By
implementation* to *Settled*, update its status and point at the decision
rather than deleting it.

(The shape is borrowed from Mojo's `edge-case-behaviors.md`; the contents
are entirely ours.)

## Slices (`T[]`)

**`null` is an empty slice, but an empty slice is not `null`.** — *Settled,
and easy to get backwards.* `null` converts to a slice with `.len == 0`
that compares equal to `null`. The converse does not hold: a zero-length
slice over real storage — `b[2..2]` of a live buffer — has `.len == 0` and
compares **not** equal to `null`, because the comparison
is on the pointer, not the length. So `if s == null` means "has no
storage", not "is empty"; test `.len` for emptiness. `free(s)` on either
kind frees the pointer, whatever the length.
[language-reference.md § Slices](language-reference.md#slices-t).

**A view over a raw pointer is unbounded, and sub-slicing it trusts you.**
— *Settled.* `p as T[]` and a `T[]` returned by a C extern have `.len ==
-1`; indexing them is unchecked, as it was before slices existed. Bounding
with an explicit end — `(p as byte[])[0..n]` — is the documented way to get
checking back, and it is the one place `[lo..hi]` performs **no** check:
there is nothing to check `n` against, so `n` is a claim the producer must
know is true. Never derive it from untrusted input.
[language-reference.md § Slices](language-reference.md#slices-t); the
constraints list on #2301.

**A slice decays to its pointer at every C boundary and the length is
dropped.** — *Settled.* A `T[]` parameter or return of an `extern` is the
bare `T*` C declares, not the `{ptr, len}` struct. A wrapper that wants the
C side bounded must pass `.len` itself, and check sizes before the decay.
[language-reference.md § Slices](language-reference.md#slices-t),
[c-interop.md](c-interop.md).

**A negative index is out of range, never a read before the buffer.** —
*Settled.* `s[-1]` is the same bounds panic as `s[s.len]`; there is no
Python-style wraparound. [language-reference.md § Slices](language-reference.md#slices-t).

**`T[]`, `make([]T, n)`, `[E]T`, `bit_set[E]` and `Isolated[T]` are
parametric, and nothing you write can be.** — *Under review.* The compiler
has parametric built-in types; there is no user-definable generic function
or struct, so `std.sort` needs `ints(int[])`, `longs(long[])` and
`floats(float[])` as three definitions. The reference says "v1 is
monomorphic; type parameters are a follow-up." Tracked in #1973 and #1050
(item 3).

## Strings and pointers

**A `string` decays to a `ptr` parameter implicitly, but `s as ptr` is a
compile error.** — *By implementation.* Passing `s` where an `extern`
declares `ptr` works (the string's data pointer is what C receives);
writing `s as ptr` is `E0200: cannot cast string to ptr with 'as'`. The
workaround when a bare pointer is genuinely wanted is a `ptr -> ptr` extern
helper that the string decays into. Nobody has decided whether the explicit
cast *should* be legal.

**An `extern` declaration is trusted, not checked against the C.** — *By
implementation.* The compiler emits the C prototype your `extern` line
implies. If it disagrees with the real one — a `-> ptr` where C says
`const char*`, a `s: string` where C says `const void*` — the failure is
the **C compiler's** `conflicting types for 'name'` pointing at the
generated file, not an Aether diagnostic pointing at your line. When that
happens, the fix is on the Aether side; the generated file is right about
what you asked for.

## Locals and inference

**A local has one type: the one its first binding gave it.** — *Settled.* A
later bare assignment of another kind is a compile error, not a re-bind. A
name first bound inside a branch or loop body and used after it is one
variable for the whole function and takes the numeric join of every
binding (`n = 1` in one arm, `n = 4000000000` in the other makes `n` a
`long`). [language-reference.md § Variables](language-reference.md).

**A parameter whose type is inferred from call sites widens to the widest
numeric kind any call site supplies.** — *Settled* (0.660, #1972). Widening
only, and only among the ranked numeric kinds; an irreconcilable pair is a
type error. Signed and unsigned 64-bit share a rank and do **not** widen
into each other, because that swap changes what a value means. Before
0.660 the first call site won and later ones were silently truncated —
that was a bug, not a rule.

## Allocation

**`make([]T, n)` calls `calloc` and panics on out-of-memory.** — *By
implementation, under review for policy.* There is no fallible `make`. Code
that today uses a cap-accounted or custom allocation path should not be
swapped to `make` mechanically; #2301's constraints call for an
allocation-policy decision first.

**A `std.bytes` handle is not its data pointer, and `length` is not
`capacity`.** — *Settled.* `bytes.new` returns a struct handle; writing
through it with `std.mem` corrupts the struct (it survives on Linux and
aborts on Windows). Use `bytes.view(b)` (the logical contents, `length`
bytes) or `bytes.capacity_view(b)` (the writable storage) — they are
different bounds. [std/bytes/README.md](../std/bytes/README.md).

## Evaluation

**`const X = expr` substitutes `expr` at every use.** — *Settled.* For a
literal this is free; for `const X = make_thing()` it re-calls
`make_thing()` at every reference, allocating fresh state each time. A
process-global initialised once belongs in `std.config` or `std.actors`.
[language-reference.md § const](language-reference.md).

**Tail calls are not turned into loops.** — *Settled.* A recursive walk
over a `*Struct` chain costs one C stack frame per cell. For a chain whose
length you do not control, walk the spine iteratively, as `*StringSeq`
does internally. [language-reference.md](language-reference.md),
[sequences.md](sequences.md).

## Adding an entry

Include: the behaviour in one sentence, a minimal example if it is not
obvious, the status, and the link (reference section, README, or issue).
If you cannot reproduce it on the current release, it does not go in — the
tracker is the place for "I think I saw…". If it is a defect, file it
instead.
