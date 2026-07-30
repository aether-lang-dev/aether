<!-- Cross-reference: Fir vs. Aether comparison (full menu of features to consider/skip). -->
<!-- Kept alongside the code so the comparison stays discoverable for future contributors. -->

# Fir vs. Aether, feature comparison for cross-pollination

> **Status:** Design-exploration survey (May 2026). A record of what was considered from Fir and what, if anything, was worth adopting into Aether, not a roadmap. Adopted items are noted inline; anything not yet shipped is a candidate, not a commitment.
>
> Source project: Fir, https://github.com/fir-lang/fir.

This survey is written for Aether's maintainers, weighing what, if anything,
from Fir is worth porting. It compares the in-tree Fir snapshot in this
directory against Aether as described in Aether's LLM.md.

This document is intentionally detailed enough that an Aether implementer can
read **only this file** and start designing a port of any individual feature
without having to re-read Fir's source.

---

## 1. One-paragraph framing

**Aether** is a Go-ergonomics / capability-disciplined / actor-flavoured
systems language that compiles to C, with two emit modes (`--emit=exe` and
`--emit=lib`) and a runtime `LD_PRELOAD`-based libc sandbox. The differentiator
is **safety-of-host**: making it cheap to embed untrusted Aether code inside C,
Python, Java, Ruby, etc.

**Fir** (osa1, https://osa1.net/, repo `fir-lang/fir`) is an
**indentation-sensitive, ML-family, typed functional language with traits, row
types, and an effect system**, also compiling to C via monomorphisation. The
differentiator is **expressiveness of the type system**: row-polymorphic
records and variants, type-classes with associated types, and a simple but
real row-typed effect system (so error sets compose at the type level).

They overlap in *implementation strategy* (a Rust front-end emitting C through
a lowered, monomorphised IR) and in their distrust of GC. They diverge sharply
in *language design philosophy*: Aether is "C with seatbelts and actors", Fir
is "Haskell with strict semantics, indentation, and unboxing". The interesting
question is therefore not "should Aether become Fir", it shouldn't, but
"which of Fir's type-system pieces have payoff disproportionate to what it
would cost Aether to keep its current shape?"

The short answer up front: **two clear wins** (row-typed open variants for
errors; `#[derive(...)]` with an open trait list), **two strong maybes**
(associated types in traits; record splicing/extension), and **several
"interesting but probably not worth the disruption"** items captured at the
end.

---

## 2. Side-by-side at a glance

| Dimension                        | Aether                                                 | Fir                                                                                  |
| -------------------------------- | ------------------------------------------------------ | ------------------------------------------------------------------------------------ |
| Family                           | Imperative, Go-shaped                                  | ML/Haskell-shaped, expression-oriented                                               |
| Syntax                           | Braces, `func(x: int) -> int`                          | Indentation-sensitive (Python-like), `f(x: I32) I32:`                                |
| Backend                          | C                                                      | C (also a tree-walking interpreter; also Wasm via `wasm-bindgen` for the playground) |
| Generics                         | (limited / under construction)                         | Monomorphised, with trait bounds                                                     |
| Polymorphism mechanism           | n/a (concrete types + interfaces TBD)                  | Traits (typeclasses) with associated types and default methods                       |
| Records / structs                | Nominal `struct`s                                      | Both **nominal** (`type Point(x:U32, y:U32)`) and **anonymous** (`(x=1, y=2)`)       |
| Sum types                        | (planned / not central)                                | First-class; pattern-matched; can be **open** via row variables                      |
| Error handling                   | Go-style `(value, err)` tuple, empty-string-on-success | Two parallel idioms: `Result[err, ok]` *or* row-typed effects (`f() T / [E1, E2]`)   |
| Effects / exceptions             | None at the type level                                 | Open row of effects in function signatures; `try` reifies to `Result`                |
| Actors                           | First-class (`receive`, `send`, declared messages)     | None                                                                                 |
| Capability sandbox               | `--emit=lib` + `--with=`, `hide` / `seal except`, LD_PRELOAD libc grant list | None                                                                                 |
| Memory model                     | Refcount + arena, RAII via codegen                     | Refcount; value types unboxed; monomorphisation enables stack/struct layout          |
| Strings                          | Length-aware internally; `string_concat` C-string-truncates | UTF-8 byte vector; `Char` is a code point; `StrBuf` builder                          |
| String interpolation             | Compile-time, simple                                   | Compile-time, `` "Hello, `name`!" `` with arbitrary expressions in backticks         |
| Pattern matching                 | Yes                                                    | Yes, with **guards**, **`is` patterns** (scoping the bound name into the guard), exhaustiveness checks, **literal string patterns**, and `..` row-patterns |
| Closures                         | Yes (compile to C)                                     | Yes, typed with effect sets; `\(): expr` lambda syntax; trailing closure syntax     |
| Iterators                        | (none surfaced)                                        | `trait Iterator` with associated `Item` type and effect parameter; ~15 combinators in `Fir/Iter.fir` |
| Stdlib surface                   | `std.fs`, `std.net`, `std.os`, `std.json`, `std.string` | `Array`, `Vec`, `HashMap`/`Set`, `Deque`, `List`, `Iter`, `Str`, `StrBuf`, `Char`, `Num`, `Option`, `Result`, `PPrint`, `Exn`, `RowToList` |
| Embedded interpreters            | Yes (Lua, Python, Perl, Ruby, Tcl, JS in-process)      | Tree-walking interpreter ships in-process (used for tests, REPL, playground)         |
| Tooling shipped in repo          | `aetherc` only                                         | `aetherc` equiv + a **PEG generator** (`Tool/Peg/`) and a **formatter** (`Tool/Format/`), both written in Fir |
| Self-hosting                     | No                                                     | Partial: a Fir compiler is being written in Fir under `Compiler/`                    |
| Tests                            | `tests/regression/*.ae`                                | `Tests/*.fir` **409 files**, golden-tested via `goldentests`                       |
| Web playground                   | No                                                     | Yes, Wasm build under `build_site` justfile target                                  |
| Runtime size                     | Hand-written C runtime per stdlib module               | Single small generated C runtime header + `setjmp/longjmp` for exceptions            |
| Pretty printer                   | Ad hoc                                                 | First-class, `Doc` ADT, `ToDoc` trait, derivable                                    |
| Comments                         | `//`, `/* */`                                          | `#`, `#| |#` (nestable!), and a **doc-comment** convention (`##`)                    |

---

## 3. Detailed feature inventory of Fir, with porting notes

For each feature: what Fir does, how it works, why it's interesting for
Aether, what the porting cost looks like, and a verdict.

### 3.1 Row-typed open variants for error sets ⭐ STRONG RECOMMEND

**What Fir does.** A function's error set is written as an open row of
zero-arg variants:

```fir
parseU32(s: Str) Result[[InvalidDigit, Overflow, EmptyInput, ..r], U32]:
    if s.len() == 0:
        return Result.Err(~EmptyInput)
    ...
```

The `~EmptyInput` syntax is "wrap this as variant `EmptyInput` of an
anonymous open variant". `..r` is a row variable: callers can extend the
error set without touching the signature. Handlers narrow the row:

```fir
defaultEmptyInput(res: Result[[EmptyInput, ..r], U32]) Result[[..r], U32]:
    match res:
        Result.Err(~EmptyInput): Result.Ok(u32(0))
        Result.Err(other): Result.Err(other)   # `other` has type [..r]
        Result.Ok(val):    Result.Ok(val)
```

The compiler knows `other` is the residual row after `EmptyInput` is
removed, so the second branch's type is `Result[[..r], U32]` the handler
*shrinks* the row at the type level. No loss of precision, no dynamic
downcasting.

**Why Aether should care.** Aether currently uses Go-style `(value, err)` /
empty-string-on-success. That works at the call site but is **opaque to
the compiler**: the compiler has no idea which errors a function can
produce, so it can't tell you that you forgot to handle `EmptyInput` after
adding it. Open variants give you exhaustiveness checks for free, including
*after* refactors that add a new error case.

This composes especially well with downstream users (e.g. svn-aether) who
need to surface dozens of distinct failure modes through layered helpers.
The current convention forces them to either re-encode error kinds in a
parsed string or use a sentinel scheme. A row variant is the right
primitive.

**Porting cost.** Non-trivial but bounded.

- Type system needs **row variables** for variants. Fir's `src/type_checker/row_utils.rs` is 133 lines plus the relevant cases in `unification.rs`. Manageable.
- Codegen: tagged union with a discriminant. Aether already emits structs; this is one more shape. Fir's `to_c.rs` lowers variants to `struct { uint64_t _tag; union { ... }; }` same trick.
- Pattern matching needs to learn open patterns: `Result.Err(~Foo)` and `Result.Err(other)`. The "other" binding's type is the parent row minus the matched constructors.
- Without first-class sum types this is hard; if Aether is going to add sum types anyway, do this *together* with that work, not later.

**What to import vs. invent.** Import the *idea* and the surface syntax
(`[Tag1, Tag2, ..r]` for open rows is concise and reads well). Don't
import Fir's exact unification algorithm sight unseen, Aether's existing
type checker has its own shape.

**Concrete files to read in Fir before designing the port.** In order:
1. `Tests/ErrorHandling.fir` the canonical example, 248 lines, runnable.
2. `Tests/Rows1.fir`, `Tests/Rows2.fir`, `Tests/Rows3.fir` minimal demos.
3. `src/type_checker/row_utils.rs` the data structure for rows.
4. `src/type_checker/unification.rs` (search for `unify_row`).

**Verdict.** This is the single highest-leverage feature in Fir for
Aether's audience. It pairs naturally with capability-discipline:
*compile-time-known error sets* are the same kind of invariant as
*compile-time-known capabilities*. Strongly recommend.

---

### 3.2 Row-typed effect system ⭐ INTERESTING; recommend a *trimmed* form

**What Fir does.** Every function type has a "exception row":

```fir
foo(f: Fn() () / exn1) () / exn2:
    f()
```

Reads as: "`foo` calls `f`, which may throw any effect in `exn1`, and
`foo` itself may throw any effect in `exn2`". The compiler enforces
`exn1` ⊆ `exn2` (you can't silently swallow exceptions).

`throw(~Foo)` adds `Foo` to the row; `try(\(): expr)` reifies the row
into a `Result[exn, a]`:

```fir
prim try(cb: Fn() a / exn) Result[exn, a]
```

So `throw` and `try` are **the only two primitives**; the row
discipline is entirely in the type system. At the C level it's plain
`setjmp`/`longjmp` (`to_c.rs:137-153`):

```c
typedef struct ExnHandler {
    jmp_buf buf;
    struct ExnHandler* prev;
    void* exn_value;
} ExnHandler;
static ExnHandler* current_exn_handler = NULL;
static void throw_exn(void* exn) { ... longjmp(...); }
```

So at runtime it's "exceptions, the C way". The cleverness is purely in
the types.

There is also a beautiful trick called **"throwing iterators"**
(`Tests/ThrowingIter.fir`, blog post linked in README): the `Iterator`
trait is parametric over the effect row, so `iter.map(parseU32Exn)` gives
back an iterator whose `next` carries the parser's exception row, and
`for x in iter:` propagates effects naturally. This makes try-each-element
patterns ergonomic in a way that Go-style returns and Rust's `?` don't
quite manage.

**Why Aether should care.** Aether has no language-level error semantics
beyond `(value, err)`. That's fine for one-call-deep code but accumulates
plumbing as call depth grows (`if err != "" return ..., err` becomes the
dominant noise). A row of effects per function gives:

- **Compile-time exhaustiveness across error kinds.**
- **Compose-by-default**: an iterator, an actor message handler, or a
  callback can be polymorphic in the row.
- **Cheap reification**: `try` already exists in spirit (Aether could
  reify a row to `(value, err)`).

**Why be cautious.** Effect rows are a real conceptual ask. The error-set
piece (3.1) gets you 80% of the win without the language having to track
*all* sources of failure separately from values. If 3.1 lands first, this
becomes a smaller delta on top.

**Porting cost.** Larger than 3.1 alone.

- Every function type grows an effect row component.
- Closure types do too, and closures are already a friction point in
  Aether's codegen.
- Pattern matching of caught errors has to know about open rows (already
  needed for 3.1).
- Need a runtime exception mechanism. Fir uses `setjmp`/`longjmp`. For
  Aether-on-C this is the obvious choice; for `--emit=lib` and the
  embedded-DSL case, longjmp across a host boundary is unsafe, `try` at
  the language root would have to catch any escape. (Mirror the
  capability discipline: an `--emit=lib` library should not propagate
  exceptions into the host. `try` at the FFI boundary becomes mandatory.)

**Trimmed form recommendation.** Adopt 3.1 (open variant errors) first.
Then, if the ergonomics push for it, add a thin effect row that's
implicitly "the error row of any `Result` in scope". Don't generalise to
arbitrary effects (state, IO, async), that's a much larger commitment
and pulls in handlers, which Fir doesn't even have.

**Files to read.**
1. `Fir/Exn.fir` the entire surface, 15 lines.
2. `Tests/FnExnTypes1.fir` and `FnExnTypes2.fir` minimal effect-row checking.
3. `Tests/ThrowingIter.fir` the motivating example.
4. `src/to_c.rs` lines 137-180, runtime is ~30 lines of C.

**Verdict.** Phase 2 work. Land 3.1 first, then revisit.

---

### 3.3 Anonymous row-polymorphic records ⭐ STRONG RECOMMEND for product types

**What Fir does.** Records are first-class **without nominal declaration**:

```fir
let r = (x = 1, y = 2)        # type: (x: Int, y: Int)
test(r: (x: I32, y: I32, ..rest)):    # accept anything with at least x and y
    print(r.x); print(r.y)
```

`..rest` is a row variable over fields. A function can accept any record
with the required fields and return a record with the same row variable
preserved:

```fir
id(r: (x: I32, ..r)) (x: I32, ..r):    # identity preserves extra fields
    r
```

You can also **splice** a record into another:

```fir
let a = (x = 1)
let b = (y = 2, ..a)               # (x: Int, y: Int)
let c = (z = 3, ..b)               # (x: Int, y: Int, z: Int)
```

And nominal types can be parameterised over a row of extras:

```fir
type Foo[extras](x: U32, y: U32, ..extras)

let foo = Foo(x = 123, y = 456, z = "hi")
foo.z = "bye"   # this works because `extras` was inferred to {z: Str}
```

And constructors can be called with a record splice:

```fir
type Point(x: U32, y: U32)
let args = (x = u32(42), y = u32(99))
let p = Point(..args)     # record-splice into nominal constructor
```

(See `Tests/FunArgSplicing1.fir`.)

There's even a **row-of-fields type synonym**:

```fir
type Foo = row(x: I32, y: I32)
makeFoo() (..Foo): (x = 1, y = 2)
```

(See `Tests/RowTypeSynonyms1.fir`.)

**Why Aether should care.** Most actor messages, JSON-shaped payloads,
function-call argument bundles, and config records have a "shape" that's
defined locally. Forcing every shape to be a nominal struct creates
top-level declaration noise and makes refactoring painful (rename a
field, update 6 declarations). Anonymous records eliminate that noise.

Specifically for Aether:
- **Actor message types.** Right now they're declared. Allowing
  `(kind = .Foo, payload = ..)`-shaped messages with row-poly handlers
  would be a real ergonomics win. **Caveat**: Aether's actor model
  intentionally uses *declared* messages (per `LLM.md`) for
  cross-process safety, so this might be a non-fit there specifically.
  Anonymous records inside a single process, fine; across the actor
  boundary, keep nominal.
- **Aether's `--emit=lib` ABI** is by-name (`aether_<fn>`). Anonymous
  records are by-shape. They don't conflict, anonymous records exist
  *inside* an `aether_<fn>`, never as the export type itself. The ABI
  contract stays nominal at the boundary; ergonomics improve inside.

**Porting cost.** Moderate.

- Type checker grows row variables over field labels (similar machinery
  to row variables over variant tags from 3.1; some sharing possible).
- Codegen: anonymous records become anonymous structs. Fir generates a
  struct per distinct shape after monomorphisation. Aether could do the
  same.
- Field-access syntax: Aether already has `r.x`. Splice (`..`) is new
  syntax to specify, but only one new token.

**What to skip.** Fir lets sum-type variants carry rows too (`Wrap(x: I32, ..extras)`), and Trait-associated types can be of row kind. That's deep; don't import it in v1. Anonymous products are the 80% win.

**Files to read.**
1. `Tests/Rows1.fir`, `Tests/Records.fir` minimal examples.
2. `Tests/RecordSplicing*.fir` eight small examples covering splicing
   shapes.
3. `Tests/ProductExtension1.fir` the named-type-with-row-extras case.
4. `Tests/FunArgSplicing1.fir` record-splice into a constructor.

**Verdict.** Strong recommend for product types. Limit scope to records;
defer the variant/associated-type generalisation.

---

### 3.4 Traits with associated types ⭐ INTERESTING; depends on Aether's polymorphism plans

**What Fir does.** Traits are typeclasses (Haskell-style; not Go interfaces):

```fir
trait Iterator[iter, exn]:
    type Item                          # associated type

    next(self: iter) Option[Item] / exn

    map(self: iter, f: Fn(Item) b / exn) Map[iter, Item, b, exn]:
        Map(iter = self, f = f)
    ...
```

Then implementations bind the associated type:

```fir
impl[Step[t], Ord[t]] Iterator[RangeIterator[t], exn]:
    type Item = t
    next(self: RangeIterator[t]) Option[Item] / exn:
        ...
```

Trait bounds appear in brackets: `max[Ord[t]](a: t, b: t) t:`. Default
methods (e.g. `Eq.__neq` defaulting to `not self.__eq(other)`) live in
the trait. Operator overloading is just trait-method desugaring (`Add.__add` is `+`, `Eq.__eq` is `==`, etc.).

There's also an **Implicit type parameter** rule: if a function uses a
type variable that isn't introduced anywhere, it's implicitly added.
(See `Tests/ImplicitTyParams.fir`: `id(a: t) t: a` works without an
explicit `[t]`.)

**Why Aether should care.** Traits give you:
- **Operator overloading without ad-hoc rules** (one pathway: `+` is `__add`).
- **Polymorphic helpers like `max`, `min`, `sort`, `format`** without
  language-level magic.
- **The mechanism to express "this iterator yields T"** (associated
  types) without having to thread a phantom type parameter through every
  call site.

The associated-type piece in particular is what makes Fir's iterator
trait readable, without it you'd see `Iterator[iter, item, exn]`
threaded everywhere instead of `Iterator[iter, exn]` with `.Item` as a
projection.

**Why be cautious.** Traits are a deep language feature. The semantics
have edge cases: orphan rules, coherence, blanket impls, default-method
dispatch with overrides, trait bounds in associated-type kinds. Fir has
**408 tests** and a sizable fraction are trait edge cases, `AssocTys1.fir` through `AssocTys21.fir`, plus `AssocFn*`, `AssocTyDefaults`, `AssocTyKinds`, etc. That's not accidental complexity, that's the actual problem space.

If Aether's plan for polymorphism is "Go-style interfaces with method
dispatch via vtable", that's a much smaller commitment and is probably
the right call for Aether's audience. Traits are great for
*expressiveness* but Aether sells *predictability*. A vtable-shaped
interface system gives you 70% of the wins (operator overloading, generic
helpers) without the 408 corner cases.

**Porting cost.** Large. This is the single biggest piece in Fir.

**Files to read.**
1. `Fir/Prelude.fir` see how operators desugar.
2. `Fir/Iter.fir` the canonical motivating example.
3. `Tests/AssocTys1.fir` through any few (these test edge cases).
4. `src/type_checker/traits.rs` (253 lines).

**Verdict.** Defer. If Aether commits to a typeclass-style polymorphism
mechanism this becomes the model. If Aether stays with interfaces, skip
associated types as a first-class feature; you can simulate them with
phantom params where it matters.

---

### 3.5 `#[derive(Trait)]` ⭐ STRONG RECOMMEND (small, high-leverage)

**What Fir does.** Annotate a type with `#[derive(Eq, ToDoc)]` and the
compiler synthesises trait impls:

```fir
#[derive(Eq, ToDoc)]
type T1[t](msg: Str, ..t)
```

The expansion is a pre-typecheck source-to-source pass
(`src/deriving/mod.rs`, ~590 lines total across `mod.rs`, `eq.rs`,
`to_doc.rs`). It walks the type's constructors and fields, emits an
`impl Eq[T1[..]]` block whose `__eq` does field-wise comparison, and an
`impl ToDoc[T1[..]]` whose `toDoc` does field-wise pretty-printing.
Currently supports `Eq` and `ToDoc`; the architecture admits more.

It handles row-polymorphic types correctly:

```
T1(msg = "hello", ()) Bool.True
T2.A(x = 42, ()) Bool.True
```

(`Tests/DerivingWithExtensionFields.fir`.)

**Why Aether should care.** Aether's stdlib already has many `_eq`,
`_format`, `_clone`, `_hash` helpers per type, hand-written. Whether or
not Aether adopts traits, deriving as a code-generation step gives the
same payoff. The general principle is "if a piece of code is
*entailed* by the type definition, the compiler should write it."

Even without traits, this could lower to plain functions: `#[derive(eq)]
type Point(x: I32, y: I32)` synthesises a `Point_eq(a: Point, b: Point) bool`. That's a one-pass mechanism, no type-system upgrades needed.

**Porting cost.** Small.
- One pre-typecheck attribute scan.
- Per supported derive, a generator function that walks the type's shape.
- Documented support list (`Eq`, `Format`, `Clone`, `Hash` is the
  obvious starter set).

**Concrete API shape for Aether (suggested).**
```aether
#[derive(eq, format)]
struct Point { x: int, y: int }

# generates:
# func Point_eq(a: Point, b: Point) -> bool: a.x == b.x && a.y == b.y
# func Point_format(p: Point) -> string: "Point(x={p.x}, y={p.y})"
```

Use plain function names (no trait machinery). Document exactly what the
generated code looks like, let users read it as if they wrote it.

**Files to read.**
1. `src/deriving/mod.rs` the dispatch pass.
2. `src/deriving/eq.rs` clean exemplar of one derive.
3. `src/deriving/to_doc.rs` slightly more involved (handles Doc).

**Verdict.** Strong recommend. Even if nothing else from this document
ships, this is high value per LOC.

---

### 3.6 Pattern matching with guards, `is`, exhaustiveness, and string literals ⭐ RECOMMEND

**What Fir does.**

- **Guards on arms**: `Option.Some(i) if i.mod(2) == 0:`
- **`is` patterns** that bind into the guard scope:
  ```fir
  Option.Some(y) if y is Option.Some(y): print(y)
  ```
  The first `y` is the outer Option's payload; if it itself is
  `Some(y)`, the inner `y` shadows in the body.
- **Exhaustiveness checking** with a quality message:
  `Tests/MatchGuards.fir:13:5: Unexhaustive pattern match`. Implemented
  in `src/type_checker/pat_coverage.rs` (795 lines, proper algorithm,
  not just "did you cover the constructors").
- **Literal string patterns**:
  ```fir
  match s:
      "asdf": ...
      "test": ...
      _:      ...
  ```

**Why Aether should care.** This is mostly orthogonal to the rest. Even
without sum types or row variants, structural pattern matching on
Aether's existing `(value, err)` returns and on numeric/string values
would be a notable ergonomics improvement. Exhaustiveness checking
provides a real safety guarantee, particularly useful in an
actor-model language where you want to ensure every message variant is
handled.

**Porting cost.** Medium. The exhaustiveness algorithm is a real
implementation effort but well-documented (Maranget's algorithm or
similar).

**Verdict.** Recommend, especially the **exhaustiveness checking** for
actor message handlers. That alone catches a class of bugs Aether
currently has no defense against.

---

### 3.7 Indentation-sensitive syntax, **DON'T DO THIS**

Fir is indentation-sensitive (Python-shaped). Aether is brace-shaped.
This is the **single most invasive language change possible**. Both
choices are defensible, neither is better, but switching costs are
enormous and the payoff is "looks slightly cleaner". Hard pass. Mention
only to mark it explicitly off the list.

---

### 3.8 String interpolation with arbitrary expressions

**What Fir does.** Backtick-delimited expression splices inside strings:

```fir
print("Hello, `name`!")
print("-`do:
    print('Hi')
    g('world')
`-")
```

Yes, that includes multi-line `do:` blocks inside the interpolation,
parsed by a small dedicated state machine in `src/interpolation.rs`.

**Aether status.** Aether already has string interpolation per `LLM.md`
("string interpolation, pattern matching"), so the question is **shape**
rather than whether to add it at all.

**Porting note.** If Aether's interpolation is currently `${expr}`-shaped
(C-template-string-like), Fir's choice is interesting because backticks
are unused punctuation in most languages and free up `${}`. Not a porting
target unless the existing surface is hitting limits.

**Verdict.** Skip. Aether's existing interpolation is fine.

---

### 3.9 Pretty-printer (`Doc` + `ToDoc`), RECOMMEND for the stdlib

**What Fir does.** A first-class pretty-printing library
(`Fir/PPrint.fir`) modelled on Wadler/Leijen's `Doc` algebra:

- `Doc.str(...)`, `Doc.nested(n, doc)`, `Doc.hardLine()`, `+` for
  concatenation, `.render(width)` to produce a `Str`.
- A `ToDoc` trait that types implement to be pretty-printable.
- `#[derive(ToDoc)]` synthesises field-wise rendering.
- Used pervasively in error messages from the compiler itself.

**Why Aether should care.** Aether's user-facing output (compiler
errors, debug printing) currently goes through ad-hoc string
concatenation. A `Doc` library gives:

- Consistent line-wrapping and indentation.
- A way to compose error messages from sub-pieces.
- A clean target for `#[derive(format)]` from §3.5.

**Porting cost.** Small. A pretty-printer is ~300 lines of
straightforward code. No language changes required, just a new stdlib
module. Wadler's "A Prettier Printer" paper is the canonical reference.

**Verdict.** Recommend. Land alongside `#[derive(format)]` from §3.5.

---

### 3.10 Built-in PEG generator (`Tool/Peg/`), INTERESTING SOCIAL PRECEDENT

Fir ships a **PEG parser generator** in `Tool/Peg/`, written *in Fir*. It
takes a `.peg` grammar file and emits Fir source. The Fir compiler's own
grammar is partly defined that way (`Compiler/Grammar.peg` →
`Compiler/Grammar.fir`).

**Aether implication.** Not a feature to port, but a **template**: a
self-hosted ecosystem tool that demonstrates the language is
expressive enough to be its own metalanguage. svn-aether is one such
demonstrator; an `aether-peg` or an `aether-fmt` would be another, and
would surface gaps in the language faster than any test suite.

**Verdict.** Not a porting target. Worth noting that Fir's authors
treat "the standard tools should be written in the language" as
load-bearing, that's a good North Star.

---

### 3.11 Doc comments (`##`) and nestable block comments (`#| ... |#`)

**What Fir does.**
- `# line comment`
- `#| block ... |#`, **nestable**: `#| outer #| inner |# more |#` parses
  correctly.
- `## doc comment`, attached to the following declaration.

**Why Aether should care.** Nestable block comments are a small but
real win when commenting out code that already contains comments. Doc
comments enable a future docgen tool. Both are zero-cost lexer changes.

**Verdict.** Cheap; recommend if Aether is comfortable changing its
comment lexer. Otherwise skip.

---

### 3.12 Go-style trailing closure / lambda syntax

```fir
let c2 = \(): c2(); c1()           # lambda
test(\() U32 / [Err]: 123)         # lambda with type and effect annotations
```

Aether already has closures per `LLM.md`. The Fir-specific note is the
lambda syntax `\(args): body` terser than `func(...) { ... }`. If
Aether wants a shorter closure form for callbacks, this is a candidate
but not material.

**Verdict.** Skip.

---

### 3.13 Things in Fir that are **not** worth porting

These are real Fir features but either don't fit Aether's audience, are
too costly relative to value, or are already covered:

- **Tree-walking interpreter** (`src/interpreter*`). Aether is
  compile-only by design. Fir keeps the interpreter for speed of test
  iteration; Aether's regression tests already run via the C compile
  loop and that's fine.
- **Wasm backend** (`build_site` justfile target). Aether could do this
  too, but the use case is "online playground", not a priority.
- **Higher-kinded types and kind inference**
  (`src/type_checker/kind_inference.rs`). Comes with
  associated-types-of-row-kind. Skip until/unless §3.4 is adopted.
- **`#[NoImplicitPrelude]`**. Useful for the language's own bootstrap;
  Aether doesn't have an implicit prelude problem.
- **`prim` declarations** (`prim panic(msg: Str) a`,
  `prim readFileUtf8(path: Str) Str`). Fir's analogue of Aether's
  `extern`. Conceptually identical; nothing to port.

---

## 4. Cross-cutting differences worth being aware of

Even if no Fir feature ships in Aether, these pairs of design choices
are worth keeping in mind so future Aether decisions don't accidentally
foreclose options:

1. **Effect rows vs. capability flags.** Aether's `--with=fs,net,os`
   gates capabilities at *compile-build-time*. Fir's effect rows gate
   them at *type-time*. They're complementary: Aether's flag says
   "this build is allowed to call fs"; an effect row would say "this
   *function* is allowed to call fs". A `func read_config() string /
   {fs}` annotation is the type-level analogue of `--with=fs`. If
   Aether ever wants per-function capability discipline (not just
   per-build), effect rows are the mechanism.

2. **Open variants vs. nominal sum types.** Aether's actor message types
   are *declared, nominal*. Open variants would let a dispatch handler
   say "I handle Foo, Bar, and pass anything else to my parent" with a
   compile-time-known residual row. That's a strict superset of the
   current actor pattern. Worth considering for in-process dispatch
   even if cross-process messages stay nominal.

3. **Trait coherence vs. extern linkage.** Fir's traits are coherent in
   the sense that there is one `impl Eq[Foo]` per program. Aether's
   `aether_<name>` ABI is inherently coherent too (one symbol). If
   Aether ever adds traits, the coherence story is roughly free; the
   per-symbol ABI mangling already implies a global namespace.

4. **Monomorphisation vs. dynamic dispatch.** Both languages
   monomorphise. Fir does it for trait calls; Aether does it for
   generics. Both pay the same code-size cost. Don't switch to dynamic
   dispatch without measuring; the C compiler is good at inlining
   monomorphised code and bad at inlining through function pointers.

5. **`Vec` vs. `Array`**. Fir distinguishes `Array[t]` (fixed-size,
   like Rust's slice or Java's array) and `Vec[t]` (growable). Aether
   should make sure these two semantics aren't conflated under one
   name.

---

## 5. Recommended adoption sequence

If everything above were on the table, this is the order to ship things
in, smallest+highest-leverage first:

1. **`#[derive(eq, format, clone, hash)]`** (§3.5).
   No type-system changes; pure code generation. Lands as a single PR.
   Gives Aether downstream users (svn-aether port etc.) a 30% LOC
   reduction in mechanical boilerplate.

2. **`Doc` + pretty-printer in stdlib** (§3.9). Pure stdlib, no
   compiler changes. Pairs with #1.

3. **Pattern-match exhaustiveness for actor message handlers** (§3.6).
   A type-checker pass; small in scope if scoped narrowly. Catches a
   real class of bugs.

4. **Anonymous record literals + record splice** (§3.3, products only).
   Type-system change, but tightly-bounded one. Single biggest
   ergonomics win for actor messages and config records. Skip the
   variants-with-rows generalisation.

5. **Open variant errors** (§3.1). Bigger scope. Land *after* anonymous
   records, since the row-variable infrastructure is shared. This
   replaces the empty-string-on-success convention for *new* code while
   keeping the old convention working in existing code.

6. **String pattern literals + match guards** (§3.6 second half). Small
   delta on top of pattern matching.

7. **(Optional, much later)** Effect rows (§3.2). Only after #5 has
   shipped and it's clear users want the next level up.

8. **(Optional, much later)** Traits with associated types (§3.4). Only
   if Aether decides its polymorphism story is typeclasses, not
   interfaces.

---

## 6. What Fir is missing that Aether has

For symmetry, these are the places **Aether is ahead**:

- **Capability sandbox**: `--emit=lib`, `--with=`, the LD_PRELOAD
  libc grant list, `hide` / `seal except`. Fir has none of this and
  no plan for it. (Fir is a research language; it's not aimed at
  hosting untrusted code.)
- **Actor model**. Fir has no `send`/`receive`. If you want
  concurrency in Fir, you write futures-by-hand or block.
- **`--emit=lib` ABI** for FFI from C / Python / Java / Ruby. Fir
  generates a self-contained executable; there's no notion of
  "stable export ABI for hosting from another language".
- **In-process embedded interpreters** (Lua, Python, Ruby, Tcl, JS).
  Fir's interpreter only runs Fir.
- **Real OS-level standard library**: `std.fs`, `std.net`, `std.os`,
  `std.json`. Fir's stdlib is data-structure-heavy (Array/Vec/HashMap/
  Iter/Str) but currently *has no file I/O at all*, the
  `ErrorHandling.fir` example notes "We don't have the standard
  library support for file IO yet". For practical work, Aether is
  much further along here.
- **Cross-platform CI** including Windows MSYS2 / MINGW-w64 (per
  Aether's `LLM.md`). Fir's CI is Linux-only.

In short: **Fir is the better-shaped language; Aether is the more
deployable one.** The features above are exactly the reason a
production user picks Aether today and wouldn't pick Fir today.

---

## 7. Bottom line

The Fir features worth Aether's attention, ranked by value-per-effort:

| Feature                                             | Section | Effort  | Value     | Verdict                        |
| --------------------------------------------------- | ------- | ------- | --------- | ------------------------------ |
| `#[derive(...)]` for eq/format/clone/hash           | §3.5    | Small   | High      | **Do this first**              |
| `Doc`/pretty-printer stdlib module                  | §3.9    | Small   | Medium    | Land alongside derive          |
| Pattern-match exhaustiveness                        | §3.6    | Medium  | High      | Recommend                      |
| Anonymous records + splice                          | §3.3    | Medium  | High      | Recommend                      |
| Open variant errors (row-typed `Result`)            | §3.1    | Med-Lg  | Very high | Recommend, after anon records  |
| String pattern literals + match guards              | §3.6    | Small   | Medium    | Cheap follow-up                |
| Doc/nestable comments                               | §3.11   | Trivial | Small     | Drive-by                       |
| Effect rows                                         | §3.2    | Large   | High      | Defer to phase 2               |
| Traits with associated types                        | §3.4    | Very large | High   | Only if polymorphism = traits  |
| Indentation-sensitive syntax                        | §3.7    | Catastrophic | Cosmetic | **No**                       |

The single thing this survey pushes hardest is **§3.1 (open variant
errors)**: it composes perfectly with Aether's existing capability discipline
("compile-time-known sets of things you can do/throw/touch"), it
addresses a real ergonomics ceiling in the current `(value, err)`
convention, and it's the entry point that makes the rest of the row
machinery cheap.

Everything else is a "yes if you have the bandwidth" rather than a
"this is the missing piece".

---

## TL;DR for triage

The full doc above is a survey of Fir, an indentation-sensitive, ML-family typed functional language with row-typed records/variants and an effect system. Like the Flux comparison it's written to let Aether decide deliberately what to absorb.

**The doc's own recommended adoption sequence (Section 5):**

1. **`#[derive(eq, format, clone, hash)]`**, small, ship first. Filed as separate issue, see cross-refs.
2. **`Doc` + pretty-printer stdlib module**, small, pairs with #1.
3. **Pattern-match exhaustiveness**, medium, real safety win for actor message handlers.
4. **Anonymous records + record splice**, medium, single biggest ergonomics win for actor messages and config records.
5. **Open variant errors (row-typed `Result`)**, bigger; the headline win that composes with capability discipline.
6. **String pattern literals + match guards**, small follow-up.
7. (deferred) Effect rows.
8. (deferred) Traits with associated types.

**Explicit skip:**
- **Indentation-sensitive syntax**, catastrophic switching cost, cosmetic value.

**The single thing the doc pushes hardest:** open variant errors (§3.1). "It composes perfectly with Aether's existing capability discipline (compile-time-known sets of things you can do/throw/touch), addresses a real ergonomics ceiling in the current (value, err) convention, and is the entry point that makes the rest of the row machinery cheap."

## Note on overlap with Flux comparison

Several items overlap with the Flux survey:
- **Pattern matching exhaustiveness**: Fir flags this stronger than Flux did.
- **Sum types / variants**: Both languages have them; Fir's row-typed variants are the more ambitious form.
- **Stringify / interp**: Fir has it via backticks; Flux has `$ident`; Aether already has interpolation.

The biggest divergence between the two surveys: **Flux pulled hard on bit-precise types (`data{N}`, `from`-cast)**. Fir pulls hard on **type-system ergonomics (rows, traits, deriving)**. They're complementary rather than competing recommendations, different parts of the language.

## What this issue is NOT asking

- Not a roadmap. "Use this as a menu, not a roadmap."
- Not asking for any single feature to ship as-listed. Each Tier 1 item should get its own issue / PR with proper review.
- Not litigating §3.7 (indentation-sensitive syntax). Correctly identified as a hard pass.

## Cross-refs

- `#[derive(...)]` (§3.5): filed as separate issue (small, low-cost, high-leverage)
- Open variant errors (§3.1): noted as the doc's headline recommendation; flag for separate issue if/when you greenlight it (it's bigger work and depends on anonymous records §3.3 landing first)
- Source project: Fir, https://github.com/fir-lang/fir.
- Sister survey: the Flux comparison (complementary recommendations)
