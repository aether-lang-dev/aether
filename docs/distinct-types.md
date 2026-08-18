# Distinct Types

> **Status:** implemented. The first cut below is live; the rest of this
> document keeps the original design rationale and the remaining **TBD** items
> for future refinement.

## Implemented

A zero-cost nominal wrapper over a scalar / `string` / `ptr` base:

```aether
type USD = distinct float
type Fd  = distinct int

charge(amount: USD) -> USD { return (amount as float * 1.1) as USD }

main() {
    let price = 9.99 as USD          // wrap: base -> distinct (explicit `as`)
    let total = charge(price)
    let cents = total as float       // unwrap: distinct -> base (explicit `as`)
}
```

- **Declaration:** `type Name = distinct Base` at top level. `type` and
  `distinct` are contextual identifiers (usable as names elsewhere).
- **Zero cost:** a distinct type lowers to its base C type, no boxing, no
  vtable. `USD` is a C `double`, `Fd` a C `int`.
- **Nominal identity:** the type checker treats `USD` as separate from `float`
  and from any other distinct type. Crossing the boundary requires an explicit
  cast: `value as USD` (wrap), `usd as float` (unwrap). The same `as` form also
  does ordinary numeric conversions.
- **Enforced at:** variable declarations / assignments (a raw `float` cannot
  initialise a `USD`, and vice-versa) **and** call-argument boundaries (a
  function taking `Fd` cannot be passed a raw `int`, nor an `EUR` where `USD`
  is wanted), the capability-token discipline the design below describes.
  (Aether's argument checking is otherwise lenient; the distinct check is
  scoped to distinct types.)
- **Base kinds:** scalars (`int`/`long`/`uint64`/`float`/`byte`/`bool`),
  `string`, and `ptr`.

Original design rationale and open questions follow.

## Goal

A zero-runtime-cost way to give Aether's structurally-typed primitives
nominal identity. The motivating example:

```aether,fragment
type Path = distinct string
type ConfigK = distinct string
type Port = distinct int
type FD = distinct int

open(p: Path) -> (FD, string) { ... }
```

A function declared `open(p: Path)` accepts only values whose static
type is `Path`. A bare `string` argument is a type error at the call
site; an explicit unwrap-and-rewrap is required. The C codegen sees
`Path` as `const char*` (no wrapper struct, no tag, no boxing); the
distinction is entirely in the type checker.

## Why this matters for Aether

Four pulls, in order of how directly the language design touches each:

1. **Capability discipline.** A `GrantedFD = distinct int` lets the
   sandbox grant-list contract (`docs/containment-sandbox.md`) be
   expressed in types. Today the contract is doc-prose; the compiler
   can't enforce that a function expecting a granted FD didn't get
   handed an arbitrary `int`. Distinct types make the narrowing
   point, where an `int` becomes a `GrantedFD` the obvious place
   to validate the grant.

2. **Composes with `Isolated[T]`**. An
   `Isolated[GrantedFD]` is a non-aliasable one-shot capability
   token. The two features stack cleanly.

3. **Helps the `(value, err)` convention.** A
   `Sanitized = distinct string` return from `validate_input(...)`
   makes it a type error to forward an unvalidated string to a sink
   that requires validation. The Go-style return convention stays;
   distinct types add a nuance that the type checker can see.

4. **Stdlib internal hygiene.** `*StringSeq` vs. `string[]` is
   already a documented footgun in `LLM.md`. A pair of distinct
   types over the underlying representations would let the compiler
   diagnose the mistake with a clear message rather than a
   lowering-time C compile error.

Two downstream projects (`avn / svn-aether`, `aether-ui`) have asked
for this in spec form (per `LLM.md` § "Working with downstream
users").

## Sketch

```aether,fragment
type Path = distinct string
type Port = distinct int

main() {
    p: Path = Path("/etc/foo")              // explicit wrap, ok
    bad: Path = "/etc/foo"                  // type error: string ≠ Path

    raw: string = p                         // type error: Path ≠ string
    raw: string = p.string                  // explicit unwrap, ok
                                            // (or `string(p)`, see TBD-1)

    port: Port = Port(8080)
    n: int = port                           // type error
    n: int = port.int                       // ok
}
```

C lowering: `Path` emits as `const char*`, `Port` as `int`. No tag,
no struct wrapper. The same ABI as the underlying type.

## Lowering, confirmed (not TBD)

A `type X = distinct Y` does **not** generate a new C type. It is a
compile-time-only nominal layer over `Y`. The `aether_<name>` ABI
across `--emit=lib` exposes the underlying C type (the consumer in
C / Python / Java sees `const char*`, not an opaque `Path`).

This matches Aether's "compiles to C" mandate and matches the
`Duration` lowering precedent (`Duration` lowers to
`int64_t` ns; the nominal `Duration` distinction is type-checker-
only). Distinct types are the generalisation: the same trick
without the unit-conversion machinery.

## Surface, confirmed (not TBD)

Declaration syntax:

```aether,fragment
type Name = distinct Underlying
```

`Underlying` is any existing type (a primitive, a struct, a typedef,
another distinct type). The result is a new type that shares
`Underlying`'s storage but is nominally separate.

This matches the issue's proposed shape and matches Nim's
`type X = distinct Y` precedent.

## Open design questions, TBD

The remaining decisions need to land before implementation can start.
Each option lists what the choice implies for users and for the type
checker.

### TBD-1: Cast spelling

How does a user explicitly convert between a distinct type and its
underlying type?

| Option | Wrap | Unwrap | Pros | Cons |
|---|---|---|---|---|
| **A. Nim-style postfix** | `Path("/etc/foo")` | `p.string` | familiar from Nim; reads left-to-right | postfix `.string` looks like a field/method access; could surprise readers |
| **B. Constructor-style both ways** | `Path("/etc/foo")` | `string(p)` | uniform syntax | conflicts with constructor syntax for structs / actors |
| **C. C-style cast** | `(Path)"/etc/foo"` | `(string)p` | matches the C mental model | Aether elsewhere rejects C-style casts |
| **D. Mixed (issue's sketch)** | `Path("/etc/foo")` | `p.string` (postfix only) | matches the issue body | only one direction is symmetric |

**Maintainer call.** Once chosen, the parser, type checker, and the
section-1 examples above need to update to reflect the chosen spelling.

### TBD-2: Operator inheritance

If `Port = distinct int`, does `port1 + port2` typecheck? Does
`port + 5`? What about `port < other_port`?

| Option | Semantics |
|---|---|
| **A. No operator inheritance by default** | `Port + Port` is a type error. Each operator must be explicitly opted into per distinct type. Strictest; matches Aether's stance on tagged-int parameter passing where a bare int passed to a Duration param errors. |
| **B. All operators inherited by default** | `Port + Port` works, gives `Port`. `Port + int` errors. Matches users' intuition; matches what `Duration + Duration` already does today. |
| **C. Nim's `borrow` opt-in** | No operators by default; opt in with `type Port = distinct int with borrow(+, -, <, ==)`. Per-type fine-grained control. |
| **D. All-or-nothing per declaration** | `type Port = distinct int with borrow` borrows everything; bare `distinct int` borrows nothing. Half-step between B and C. |

**Maintainer call.** The Duration precedent chose option
**B** behaviour (`Duration + Duration` → `Duration`, `Duration *
scalar` → `Duration`, mixed comparisons rejected). Consistency
suggests this is the default to extend; the question is whether to
also offer C's per-operator opt-in for cases that want stricter
control.

### TBD-3: Extern signature interaction

An extern declares a raw C type. What types can it carry?

| Option | Where the distinct wrapping happens |
|---|---|
| **A. Extern carries underlying only; wrapper does the cast** | Matches the existing `_raw` / wrapper-suffix pattern. `extern foo_raw(p: string) -> int` + `foo(p: Path) -> int { return foo_raw(p.string) }`. Keeps externs C-literal; the distinct discipline is an Aether-side layer. |
| **B. Extern can mention the distinct type** | `extern foo(p: Path) -> FD`. Type checker unwraps for codegen. Reads more naturally; risks erasing the boundary between Aether-side and C-side type discipline. |

**Maintainer call.** Option A matches existing stdlib conventions
(`_raw` pattern). Option B would be slightly nicer at call sites but
muddles the trust boundary at the FFI.

### TBD-4: Pattern matching destructure

Should `match p { Path(s) => ... }` work as a destructuring form, or
should users go through the cast?

| Option | Spelling |
|---|---|
| **A. Implicit deref in match arms** | `match p { Path(s) => println(s) }` |
| **B. Explicit cast inside the arm body** | `match p { Path => println(p.string) }` (only the type matches; user unwraps if needed) |
| **C. No pattern support at all** | Distinct types can't be matched. Users go through `if`-on-type or explicit casts. |

**Maintainer call.** Option A is the most ergonomic but introduces a
new syntactic form. Option C is the most conservative, Aether's
pattern matching is currently struct-focused (`docs/...`), and adding
distinct-type arms is a parser+typechecker change.

### TBD-5: Format-string / interpolation behaviour

Does `"path: ${p}"` work when `p: Path = distinct string`? Or does it
need `"${p.string}"` (explicit unwrap)?

| Option | Behaviour |
|---|---|
| **A. Implicit unwrap in interpolation** | `"${p}"` prints the underlying value. Treats `${...}` as "display me" rather than "preserve nominal identity". |
| **B. Require explicit unwrap** | `"${p}"` is a type error; `"${p.string}"` works. Forces the user to acknowledge they're flattening the distinction. |

**Maintainer call.** Option A is friendlier; option B is more in line
with the strictness option B suggests for parameter passing. The
choice may track TBD-2 (operator inheritance), if operators are
inherited by default, interpolation probably should be too.

### TBD-6: Layered distinct (distinct over distinct)

Is `type Inner = distinct Path` legal where `Path = distinct string`?
If so, does `Inner` need to unwrap through `Path` first, or can it go
directly to `string`?

| Option | Unwrap path |
|---|---|
| **A. Single-step unwrap** | `Inner.string` works directly (skips `Path` layer). |
| **B. Layer-by-layer unwrap** | Must write `inner.Path.string`. |
| **C. Disallow layered distinct entirely** | A distinct type's underlying must be a non-distinct type. |

**Maintainer call.** Option A is what Nim does. Option C is the
simplest implementation. Option B is the most pedantic.

## Implementation phasing

Once the TBDs are resolved, the implementation can sequence as:

1. **Parser**: `type X = distinct Y` syntax. New AST node or
   annotation on the existing `type` declaration node.
2. **Type checker**: nominal-inequality rule. The cast surface from
   TBD-1. The operator-inheritance rule from TBD-2.
3. **Codegen**: confirm zero-cost lowering. `Path` should emit
   identical C to a bare `string` at every use site (variable decls,
   function params, return slots, ABI `--emit=lib` boundary).
4. **Stdlib pilot**: introduce `Path`, `FD`, `Port` (or some
   subset) once the language feature lands.

Each phase gets its own follow-up issue.

## References

- `Isolated[T]` composes with distinct types.
- `Duration`, the first tagged-int type in Aether;
  serves as the operator-inheritance precedent (see TBD-2).
- Bare-int → Duration at parameter passing is rejected;
  the strictness precedent for distinct types at call sites.
- `LLM.md` § "Runtime sequence of strings → `*StringSeq`, not
  `string[]`", exactly the class of mistake distinct types diagnose.
- `LLM.md` § "Ownership of `ptr`-typed returns",
  borrowed-vs-ref-counted is another candidate distinction.
- `docs/containment-sandbox.md` capability tokens, the prime use
  case.
- Nim manual § "Distinct type", reference design including the
  `borrow` annotation (TBD-2 option C / D).
