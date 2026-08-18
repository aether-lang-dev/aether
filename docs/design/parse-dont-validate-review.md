# Parse, Don't Validate, the Aether Way

One rule, three words, older than any of the languages that argue about it:

> **Parse, don't validate.** Make the type system carry the proof, not your memory.

A **validator** asks "is this okay?", answers `true`/`false`, and then forgets
what it learned the instant it returns. Three call-frames later a `string` is
just a `string` indistinguishable from `""` so you check it again, or you
hope. A **parser** takes a less-precise value and returns a *more*-precise one:
the check and its result are the same act, and the result is a type the rest of
the program can trust without re-checking.

The difference is purely *what survives the check*. Both of these do the same
work; only one keeps the receipt:

```aether,fragment
// VALIDATE, the proof evaporates. The bool is discarded; `s` is still `string`.
is_nonempty(s: string) -> bool { return string.length(s) > 0 }

// PARSE, the proof is the return type. To hold a NonEmpty you MUST have checked.
type NonEmpty = distinct string
parse_nonempty(s: string where string.length(s) > 0) -> NonEmpty { return s as NonEmpty }
```

Aether is built to make the second one the path of least resistance. This guide
is how.

## Why Aether is a good language for this

Most discussion of "parse, don't validate" is about wrestling a *structurally
typed* language (TypeScript: `string is string is string`, no real newtype) into
faking nominal identity with phantom-`unique symbol` brands and `as Email` casts
the type system merely tolerates. Aether skips that whole fight. The three things
those workarounds approximate are first-class here:

| What the technique needs | What Aether gives you |
|--------------------------|------------------------|
| A type distinct from its base that can't be forged | **`type X = distinct Base`**, a real, zero-cost newtype |
| A checked smart constructor | a normal function returning that type, optionally **`where`-guarded** or returning **`(value, err)`** |
| A wall between "outside data" and "trusted data" | the compiler refuses to cross a `distinct` boundary without an explicit `as` |

So in Aether the slogan reads concretely: **parse a `string`/`int`/`ptr` from the
outside world into a `distinct` domain type at the boundary, and every function
downstream that takes that type is handed the proof for free.**

## The two moves: strengthen the argument, or weaken the result

There are exactly two honest ways to make a function total, to stop it lying
about inputs it can't actually handle. Both keep the proof; they differ in
*where* the obligation lands.

**Weaken the result**, return `(value, err)`. The function admits it might fail
and hands the caller both outcomes. Use this for **untrusted input** that can
legitimately be malformed (a config string, a network field, user text):

```aether,fragment
type Email = distinct string

parse_email(raw: string) -> (Email, string) {     // empty err = success
    if string.contains(raw, "@") != 0 { return raw as Email, "" }
    return "" as Email, "missing @"
}
```

**Strengthen the argument**, demand the precise type. The check happens *once*,
where the value is constructed; every consumer downstream is redundant-check-free;
and if an upstream stops guaranteeing the property, the program **stops
compiling**. Use this for **invariants the rest of your code should be able to
assume** (non-empty, in-range, already-authorized):

```aether,fragment
type Age = distinct int

parse_age(raw: int where raw >= 0 && raw <= 150) -> Age { return raw as Age }

// Consumers take the strong type. A raw int can never reach this function.
birthday_message(a: Age) -> string { return "you are ${a as int}" }
```

The strengthen-the-argument move is the one worth internalizing, because it's the
one that turns a *runtime* check into a *compile-time* guarantee. Pass a raw value
where a `distinct` is required and you get a compile error, not a crash:

```aether,fragment
birthday_message(30)
// error[E0200]: Argument 1 'a' of 'birthday_message': expected int, got int,
//   a distinct type needs an explicit `as` cast at the boundary
```

That error is the entire point of the technique, delivered by the compiler. The
check can't drift out of sync with its use, because the use won't type-check
without it.

## The smell to hunt for: `-> bool` and bare side-effect checks

The single most useful instinct: **be suspicious of any check whose result you
can ignore.** A `check_x(v) -> bool` (or a function that just throws/aborts and
returns nothing) is omittable, drop the call and the code still compiles,
because nothing depends on its result. That's not a style nit; it's how the
"impossible" case sneaks back in months later when someone deletes the guard
upstream.

```aether,fragment
// SMELL, omittable. Nothing forces a caller to consult this.
ensure_nonempty(s: string) -> bool { return string.length(s) > 0 }

// FIX, load-bearing. To proceed you NEED the result, so the check can't be skipped.
parse_nonempty(s: string where string.length(s) > 0) -> NonEmpty { return s as NonEmpty }
```

Practical rule: when you catch yourself adding the *third* defensive `if` across
three files all checking the same thing, you validated where you should have
parsed. Replace the `-> bool` checker with a `-> DistinctType` parser, change the
downstream functions to take the distinct type, and let the call sites fail to
compile *upward* until you reach the real boundary, the one place the raw value
genuinely enters. Do the parse there, once.

## Name the two states: raw in, trusted out

The cleanest large-scale version is to give the untrusted shape and the trusted
shape **different names**, with a parser as the only bridge. Illegal states then
can't be represented: a trusted value simply has no constructor that skips the
check.

```aether,fragment
import std.string

type Email = distinct string
type Age   = distinct int

struct RawUser   { email: string, age: int }   // straight off the wire, suspect
struct ValidUser { email: Email,  age: Age  }   // earned trust, safe everywhere

// Each field-parser carries its own precondition in the signature, so the
// constructor below is unconditional, there is no way to build a half-valid
// ValidUser, because a failing input never reaches the struct literal.
parse_email(raw: string where string_contains(raw, "@") != 0) -> Email { return raw as Email }
parse_age(raw: int where raw >= 0 && raw <= 150) -> Age { return raw as Age }

parse_user(u: RawUser) -> ValidUser {
    return ValidUser { email: parse_email(u.email), age: parse_age(u.age) }
}

// This signature is a guarantee: it is impossible to call with unparsed fields.
send_welcome(u: ValidUser) -> string { return "welcome ${u.email as string}" }
```

`send_welcome(ValidUser)` is safe by construction. There is no path that produces
a `ValidUser` without going through the parsers, because its fields are `Email`
and `Age` and those types have only one door each. The proof lives in the field
types, enforced, not merely documented. (If you need to *recover* from bad input
rather than panic, give the field parsers `-> (Email, err)` shapes and thread the
errors; the `where`-guarded form here is the right one when malformed input is a
programmer error, not an expected case.)

## Where this pays off most: "config IS code" → "config IS *parsed* code"

Aether's pitch for server-shaped libraries is **config IS code** (see
[`closures-and-builder-dsl.md`](../closures-and-builder-dsl.md) and LLM.md): don't
ship a YAML loader, expose your start-surface as a closure-DSL block and let the
operator's config be a `.ae` file they `ae run`. The builder-DSL machinery makes
that block *read* declaratively and *wire* itself.

But an operator-authored `.ae` config block **is outside data**, King's blob
from the wire, just written in your own syntax. A setter that takes a raw string:

```aether,fragment
set_release("21")          // "21" stays a bare string forever, the validator smell
```

…throws the proof away. The upgrade is to make **the DSL setters the parsing
boundary**, so the filled config object holds domain types, not raw strings:

```aether,fragment
type Version = distinct string
parse_version(s: string where string.length(s) > 0) -> Version { return s as Version }

set_release(parse_version("21"))   // the config now holds a Version
```

The builder function body that consumes the config then can't be handed an
unparsed value, the boundary parsed once, and the types carry it the rest of the
way. Two named states, one parser between them: the operator's block is the *raw*
side, the builder's filled config is the *trusted* side.

This is the same shape as Aether's **sandbox** model, one level up. The danger
"parse, don't validate" warns against is *shotgun parsing*: checks scattered
through processing code so a program acts on a valid *prefix* before hitting an
invalid suffix, leaving state it can't roll back. A two-phase
**parse-then-execute** structure prevents exactly that. Aether's `--emit=lib` capability gate
and the LD_PRELOAD grant list are that structure at the program scale:
*gate-then-run* is *parse-then-execute*. Deciding up front what untrusted `.ae`
may reach, before it touches anything real, is the same security argument King
makes for parsing input up front, applied to capabilities instead of values.

## A complete boundary, end to end

Pulling the moves together, recoverable parse for the malformable field,
`where`-guard for the invariant, distinct types so the consumer is safe:

```aether
import std.string

type Email = distinct string
type Age   = distinct int

parse_email(raw: string) -> (Email, string) {
    if string.contains(raw, "@") != 0 { return raw as Email, "" }
    return "" as Email, "missing @"
}
parse_age(raw: int where raw >= 0 && raw <= 150) -> Age { return raw as Age }

send_welcome(e: Email, a: Age) -> string {
    return "welcome ${e as string} (${a as int})"
}

main() {
    em, err = parse_email("ada@example.com")
    if err != "" { println("reject: ${err}"); return }   // fail at the boundary…
    ag = parse_age(30)
    println(send_welcome(em, ag))                          // …trusted from here on
    // -> welcome ada@example.com (30)
}
```

Every check happens once, at the top. Below the boundary there are no defensive
`if`s, no re-validation, no "this should never happen" branches, the types
already ruled those out.

## The toolkit, in one place

| Need | Reach for | Notes |
|------|-----------|-------|
| A domain type distinct from its base | `type X = distinct Base` | zero-cost; crossing in/out needs an explicit `as` |
| Parse untrusted input that may be malformed | a `-> (X, err)` function | stdlib convention; `""` err = success |
| Enforce an invariant at the boundary | a `where`-guarded parameter | runtime-checked; violation is a loud panic naming the condition |
| Make the consumer safe by construction | take the `distinct` type as a parameter | raw values fail to compile (E0200) → the check can't drift |
| Handle a parse result honestly | `(value, err)` + `match` | no exception hiding in the call stack |
| Two named states | a `Raw*` struct and a `Valid*` struct | one parser between them; illegal state unrepresentable |

The discipline is still yours, nothing *forces* you to take a `distinct` type at
a boundary. But Aether is the rare language where, once you reach for it, the
compiler insists the rest of the way. Most languages let you do the right thing.
Aether nudges, and then holds the line.

---

*Lineage: Alexis King, ["Parse, don't validate"](https://lexi-lambda.github.io/blog/2019/11/05/parse-don-t-validate/)
(2019, Haskell); Christian Kjær Larsen, ["Parse, Don't Validate, In a Language
That Doesn't Want You To"](https://cekrem.github.io/posts/parse-dont-validate-typescript-edition/)
(2026, TypeScript). All Aether examples in this document compile and run as
shown; the E0200 errors are the actual compiler output.*
