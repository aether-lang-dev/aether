<!-- Cross-reference: Flint vs. Aether comparison (full menu of features to consider/skip). -->
<!-- Kept next to the code so the comparison is discoverable for future contributors. -->

# Aether vs Flint, feature-by-feature, with reimplementation notes

> **Status:** Design-exploration survey (May 2026). A record of what was considered from Flint and what, if anything, was worth adopting into Aether, not a roadmap. Adopted items are noted inline; anything not yet shipped is a candidate, not a commitment.
>
> Source project: Flint, <https://github.com/flint-lang/flintc>.

A side-by-side comparison of [Aether](../../README.md) and [Flint](https://github.com/flint-lang/flintc) (compiler:
`flintc`, source ext: `.ft`). Written for the Aether maintainers so that
attractive missing pieces can be ported across the C-vs-LLVM-IR codegen gap.

This is **harder than other comparisons** because Flint emits LLVM IR via the
LLVM C++ API and embeds `lld` for linking, Aether emits portable C and shells
out to the system C compiler. Anything lifted from Flint has to be re-derived
in C terms. Where that is nontrivial, this survey calls it out in the "C-codegen
reimplementation note" boxes.

## At a glance

| Axis | Aether | Flint |
|---|---|---|
| Implementation language | C | C++17 |
| Codegen target | C source → system `cc` | LLVM IR → embedded `lld` (LLVM 21.1.8) |
| Memory model | Refcount + arenas, RAII via codegen | DIMA (Detective Investigative Memory Allocation), automatic, transparent |
| Surface syntax | Go-ish: braces, `func name(x: int) -> int` | Python-ish: indentation **+** statement `;` terminators |
| Concurrency | Erlang-style actors (`send`/`receive`) | None visible in the wild, single-threaded model |
| Capability story | `--emit=lib` + `--with=fs,net,os`; `hide`/`seal except` per-block; `LD_PRELOAD` libc-call gate | None, Flint is a host-trust language |
| Sandboxed embedding | `contrib.host.<lang>.run_sandboxed(perms, code)` for Lua/Python/Perl/Ruby/Tcl/JS | None |
| C interop | `extern "C"` declarations; user writes the C, Aether sees signatures | **FIP** (Flint Interop Protocol): out-of-process bindings generator that auto-derives `.ft` stubs from C headers via per-language modules |
| Sum types | (no native) | `variant V: i32, f32, bool;` + tagged variants |
| Errors | Go-style `(value, err string)`; empty string = ok | First-class `error ErrFoo: A, B;` hierarchies, `throw`/`catch`, error sets in signatures, `type_id`/`value_id`/`message` |
| Optionals | None, wrappers return `(T, err)` | First-class `T?`, `!`, `?.`, `??`, `none` |
| Composition model | Functions over plain `data` types | **DCMP** (Declarative Composable Modules Paradigm): `data` + `func requires(D)` + `entity` |
| Tuples | Cross-FFI tuples are an open issue (split-accessor pattern via TLS) | Native `(a, b, c)` returns + `data<T1,T2,T3>` typed tuples + `.$0`/`.$1` accessors |
| SIMD primitives | None | `i32x2/3/4/8`, `f32x4/8`, `u8x4`, `f64x4`, etc., first-class with arithmetic + interpolation |
| Bitset primitives | None | `bool8/16/32/64` with `.$0..$N` bit access |
| String interpolation | Yes | Yes (`$"..."`), and works on every primitive incl. SIMD vectors with no spec |
| Tests as syntax | Standalone `tests/regression/*.ae` files; ad hoc | `test "name": …` is grammar; `flintc --test` builds a test runner |
| Build mode flags | `--emit=exe` / `--emit=lib` + `--with=fs,net,os` | `--file`, `--test`, `--out`, `--flags=`, `--no-colors` |
| Editor support | (whatever ad hoc) | LSP in-tree (`fls/`), ships with the compiler |

## What is worth lifting (ranked)

These are the items to reach for first. Each links to its dedicated section
below.

1. **[Optionals (`T?`, `?.`, `!`, `??`)](#1-optionals)**, small surface, huge
   ergonomic payoff, fits cleanly into Aether's existing `(value, err)`
   convention.
2. **[FIP-shaped C interop](#2-fip-shaped-c-interop)**, even a stripped-down
   "parse C header → emit Aether stubs" tool would replace a lot of the
   `extern fs_foo_raw` boilerplate. The split-accessor TLS pattern in `LLM.md`
   is exactly the kind of friction this kills.
3. **[Variant / tagged-union types](#3-variant--tagged-union-types)**,
   Aether has none. Codegen as C tagged union + discriminant; pattern-match in
   `match` (which is already a reserved keyword!).
4. **[Test syntax in-grammar](#4-test-syntax-as-grammar)**, the `test "name":`
   block + `flintc --test` flow is genuinely nicer than per-file regression
   harnesses. Low cost in the parser.
5. **[String interpolation across all primitives incl. SIMD](#5-string-interpolation-coverage)**
   Aether already has interpolation; Flint's interpolation just *covers more
   types out of the box*, which is mostly a runtime extension.
6. **[Grouped field access/assign `v.(x, y, z) = v.(z, x, y)`](#6-grouped-field-accessassign)**
   pure syntactic sugar that desugars to a tuple-temp; no runtime change.
7. **[Switch expressions](#7-switch-expressions)**, `let x = switch e: A -> 1; B -> 2;`
   collapses a lot of code Aether currently writes with intermediate `var`s.
8. **[DCMP / entity model](#8-dcmp--entity-composition)**, interesting but big.
   Probably not worth taking on unless Aether wants a full OOP-replacement story.
9. **[Bitset and SIMD primitives](#9-simd--bitset-primitives)**, `i32x4` etc.
   are wonderful but require codegen-to-C-vector-extension or an emulation
   layer, which is a whole subproject.

The rest of the doc is the long form, feature, Flint's syntax, concrete
codegen-to-C notes, and where it would land in Aether's existing conventions.

---

## 1. Optionals

### Flint surface

```rs
i32? maybe = 69;          // wrap
i32 unwrapped = maybe!;   // force-unwrap; trap if none
maybe = none;             // sentinel literal
assert(maybe == none);
i32 res = maybe ?? 7;     // null-coalesce
res = 5 + maybe ?? 7;     // composes with arithmetic

// Chain assignment: no-op if optional is none
v2m?.x = 5;

// Chain access yields wrapped type:
i32? xm = v2m?.x;         // i32? (none-propagating)
i32  x  = v2m?.x ?? 0;    // i32   (defaulted)

// switch arms
switch maybe:
    none: ...
    v:    ...             // v is unwrapped i32

i32 r = switch maybe:
    none -> 0;
    v    -> v;
```

For data fields too, `Vec2? vm = v;` then `vm!.x`.

### Why it's a fit for Aether

Aether's current `(value, err string)` convention is good for *fallible*
operations but oversells when the intent is just "maybe a value." It forces
synthesizing `(value, "missing")` sentinels everywhere. `T?` is strictly
additive, the Go-style returns stay for I/O, with optionals layered on top.

### Codegen-to-C reimplementation note

Three viable representations:

1. **Pointer-as-optional** for ref-typed `T?` (strings, struct pointers): the
   existing `AetherString*` already has a sentinel-value convention; `none`
   becomes a tagged null variant. **Cheapest path.**
2. **Tagged struct** for value-typed `T?`:
   ```c
   typedef struct { bool has; T val; } ae_opt_T;
   ```
   Codegen needs one such struct per concrete `T`. Mangling: `ae_opt_<typename>`.
   Already aligned with how Aether codegen instantiates types.
3. **Sentinel-bit** in the type itself for primitives where one bit pattern is
   reserved (e.g. `i32?` could use `INT32_MIN` but this is fragile, skip it).

`?.` chain access compiles to `(opt.has ? opt.val.field : <default>)`. `?.`
chain *assignment* compiles to `if (opt.has) opt.val.field = rhs;`. `!` traps
via `aether_panic("forced unwrap of none")` matching the existing panic
infrastructure.

`switch e: none: …; v: …;` desugars to:
```c
if (!e.has) { /* none arm */ } else { T v = e.val; /* v arm */ }
```

Pure-expression form (`x = switch …: A -> 1; …`) compiles to a C ternary chain
or a GCC statement-expression `({ … })` for non-trivial arms. Aether already
has both available since it emits C99 + GCC extensions on the systems
path.

### Reserved-keyword note

Aether has `match` already reserved. Don't introduce a new keyword for
optional-switch, reuse `match`, with `none`/`some(v)` arms. Picks up
ML/Rust intuition without colliding.

---

## 2. FIP-shaped C interop

This is Flint's most genuinely novel piece. Worth the most attention.

### Flint surface

User project layout:
```
my_project/
├── main.ft
├── extern.c                 # the C the user wrote
└── .fip/
    ├── config/
    │   ├── fip.toml         # turns FIP on per language
    │   └── fip-c.toml       # per-language config (sources/headers/cmd)
    └── generated/
        └── c.ft             # auto-generated Flint binding stubs
```

`.fip/config/fip.toml`:
```toml
[fip-c]
enable = true
```

`.fip/config/fip-c.toml`:
```toml
[c]
headers = ["extern.c"]
sources = ["extern.c"]
command = ["gcc", "-c", "__SOURCES__", "-o", "__OUTPUT__"]
```

User's `main.ft`:
```rs
use Core.print
use Fip.c                   # imports everything from auto-generated c.ft

def main():
    s1 := MyStruct(-112, 22.1, 33_302);
    add_structs(&s1, MyStruct(12, 17.9, 2_698));
    print($"s1.(x, y, z) = ({s1.x}, {s1.y}, {s1.z})\n");
```

Compiler at parse time talks IPC to the `fip-c` module (which parses
`extern.c`'s headers). The fip-c module returns the struct/enum/function
inventory. Compiler writes `.fip/generated/c.ft`:
```rs
data MyStruct:
    i32 x;
    f32 y;
    u64 z;
    MyStruct(x, y, z);

enum MyEnum:
    VAL1 = 0,
    VAL2 = 1,
    VAL3 = 2,
    VAL4 = 4,
    VAL5 = 8;

extern def print_enum(const MyEnum e);
extern def add_structs(mut MyStruct* s1, const MyStruct s2);
extern def add(mut i32* lhs, mut i32 rhs);
```

Then a user's hand-written `extern def add(i32 x, i32 y) -> i32;` gets *checked
against* the FIP-resolved symbol, signature mismatch is a compile-time error
(`"Extern function could not be found in any FIP module"`).

### Other primitives FIP brings

- **`opaque` type**, for `void *` / handle-style values. Aether currently
  uses `ptr`; `opaque` is the same concept but with a dedicated type that
  comparison ops handle (`if con.value == null:`).
- **Tags / aliasing**, multiple C headers can be tagged and aliased
  (`use Fip.c as ext1`). `.fip/generated/<tag>.ft` per tag.
- **Leak detection**, when the project links against an opaque-returning
  C function and the binding never sees a matching free, FIP emits
  `"Error: Leaking memory!"` at runtime exit. Hooks into the same accounting
  as DIMA.
- **Pointer rules**, `T*` is **only** legal in `extern def` signatures or
  bodies of `extern` functions. In native Flint code `T*` is rejected
  (`"Pointer types are not allowed in non-extern functions"`). This is a hard
  separation enforced in the analyzer.

### Why it's a fit for Aether

Aether's `extern fs_foo_raw() -> string` + the split-accessor TLS pattern
documented in `aether/LLM.md` exists exactly because Aether can't synthesize
length-aware bindings automatically. FIP-style header introspection would
enable:

- Emit Aether stubs for C ABI-stable libraries automatically.
- Verify that hand-written `extern` matches the C reality at compile time.
- Eliminate the `_raw` + `_get_*` + `_get_*_length` + `_release_*` quartet for
  any function whose signature can be parsed from a header.
- Keep `--with=fs` / capability story, FIP module is just another stdlib gate.

### Codegen-to-C reimplementation note

**Aether has the easier job here, not harder.** Aether already emits C, the FIP
output (`.fip/generated/c.ft` is just Aether-syntax declarations) drops in as
a normal `.ae` file with `extern` decls.

Minimum viable path:

1. **`aether-bindgen`**: a separate tool. Parses a C header (libclang, or a
   trimmed-down hand-rolled C parser; full preprocessor coverage is not
   required). Emits a `.ae` file with `extern` decls + matching `data` types.
   Same shape as `.fip/generated/c.ft`.
2. **`.fip` directory convention**: keep it. `aetherc` learns to consult
   `.fip/config/fip-c.toml` at parse time, invoke `aether-bindgen` if cache
   stale, treat the generated `.ae` as an implicit import for `use Fip.c`.
3. **Signature verification**: when user writes a manual `extern def`, look it
   up in the latest generated table; mismatch → diagnostic with the expected
   signature inline. (Flint does this; the diagnostic is one of the most
   useful UX wins they have.)
4. **`opaque` type**: introduce alongside existing `ptr`. Same lowering
   (`void *`), but type-check rules enforce that `opaque` only crosses extern
   boundaries, not assignable to/from non-extern variables. This matches
   Flint's "no `T*` in non-extern code" rule.
5. **No IPC needed initially**. Flint went IPC because it wanted a plugin
   architecture for many languages (Java/Python/Ruby). For Aether's purposes
   "C only, in-process" is already a giant win. Defer multi-language until it
   is actually needed.

Skip for now: Java/Panama, Python ctypes, Ruby Fiddle FIP modules, Aether
already covers those via `--emit=lib` + ABI-stable `aether_<name>` exports. The
direction in FIP is *ingest*; Aether's stays *export*. The two should not be
conflated.

### Where it lands in Aether's invariants

- **Capability discipline**: `use Fip.c` is treated like `std.fs` banned
  under `--emit=lib` by default, opt-in via `--with=fip-c`. Reason: untrusted
  Aether code that can synthesize C bindings is a backdoor around `--with=fs`.
- **`docs/emit-lib.md`** would gain a section "FIP and the lib emit mode."

---

## 3. Variant / tagged-union types

### Flint surface

```rs
// Untagged: types are the discriminator
variant V: i32, f32, bool;

V my_var = i32(5);
switch my_var:
    i32(i): assert(i == 5);
    f32(f): ...;
    bool(b): ...;

// Tagged: explicit tag + payload
variant VarTag:
    Int(i32), Float(f32), bool;

VarTag t = i32(10);
i32 r = t!(VarTag.Int);   // forced extraction; trap on mismatch
i32? r = t?(VarTag.Int);  // optional extraction

// Mixed-payload
variant VarTup:
    Int(i32), Tuple(f32, i32), bool;

VarTup x = (3.4, 2);      // structural match against Tuple(f32,i32)
switch x:
    VarTup.Int(i): ...
    VarTup.Tuple(t):
        assert(i32(t.$0 - 3.4) == 0);
        assert(t.$1 == 2);
    bool(b): ...
```

Switch as expression form: `i32 r = switch v: i32(i) -> i + 3; ...;`

### Why Aether needs this

Aether has none. The `(value, err)` convention covers binary outcomes; for
n-ary outcomes (parser tokens, AST nodes, JSON values) Aether currently rolls
hand-tagged structs in C and leans on convention. JSON especially is a
canonical multi-arm sum type.

### Codegen-to-C reimplementation note

Standard tagged-union lowering:
```c
typedef enum { VAR_I32, VAR_F32, VAR_BOOL } V_tag;
typedef struct {
    V_tag tag;
    union {
        int32_t i32_val;
        float   f32_val;
        bool    bool_val;
    };
} V;
```

Force-extract `v!(V.Int)` →
```c
({ assert(v.tag == VAR_I32); v.i32_val; })
```

Optional-extract `v?(V.Int)` returns the optional struct from §1:
```c
(v.tag == VAR_I32 ? (ae_opt_i32){true, v.i32_val} : (ae_opt_i32){false, 0})
```

Switch over variant lowers to a C `switch(v.tag)` with bound names per arm:
```c
switch (v.tag) {
    case VAR_I32: { int32_t i = v.i32_val; /* arm */ break; }
    case VAR_F32: { float   f = v.f32_val; /* arm */ break; }
    case VAR_BOOL:{ bool    b = v.bool_val; /* arm */ break; }
}
```

Exhaustiveness check is a parser/analyzer obligation, like Flint already does.

**Naming the keyword**: Aether has `match` reserved already (per `LLM.md`).
`variant V: …;` for declaration, `match v: …;` for use. Consistent with how
`message` is reserved for actor model.

### Interaction with actor messages

Aether's actor model declares message types. Those are *closed* sums
(per-actor mailbox spec). `variant` would generalize the same machinery into
a value-level construct. Worth scoping: should the codegen path be shared?
Probably yes, message-typed fields in receivers already lower to discriminated
unions; `variant` is the same shape exposed to user code.

---

## 4. Test syntax as grammar

### Flint surface

Files that opt into testing get parsed differently when `flintc --test` is
passed. The grammar accepts:

```rs
use Core.assert
use "utils.ft"

#test_performance
test "fsh":
    str path = "fsh";
    test_compiles_in(path, "main.ft", none);

#test_should_fail
test "[f]15_infer.ft":
    test_file_ok($"{path}/15_infer.ft", "special error thrown\n");
```

- `test "name":` is a top-level definition just like `def`.
- `#test_performance` and `#test_should_fail` are annotations.
- `flintc --test --file tests.ft --out test_runner && ./test_runner` runs them.

The example test in `examples/utils.ft` shows test helpers being defined as
plain Flint functions (`test_file_ok`, `test_file_fail`, `test_file_crash`,
etc.) and called from `test "…":` blocks. The whole testing framework is just
Flint code, there's no `assert.h`-shaped runtime.

### Why it's a fit for Aether

`tests/regression/*.ae` is fine, but it's per-file and ad-hoc, naming
conventions encode metadata (`fix_block_scope_restoration.ae`). Inline `test`
blocks would let multiple cases share file context and allow annotating
expected failures or perf-only tests in-grammar.

### Codegen-to-C reimplementation note

Cheapest implementation:

1. Parser: `test "<string>": <block>` becomes a `TestNode` AST. Annotations
   `#test_performance`, `#test_should_fail` attach to it.
2. Under `--test`, the codegen path emits a `main()` that calls each
   discovered `TestNode` body in registration order, captures asserts/panics
   per-test, prints a TAP-shaped summary.
3. Under `--emit=exe` (the default), `test` blocks are skipped, they don't
   reach codegen. This matches Flint: `flintc --file foo.ft` builds a normal
   binary regardless of how many `test` blocks `foo.ft` contains.
4. `aether_assert(cond, msg)` is already in the runtime; reuse it.

Total cost is small. Comment-only thing to watch: existing `test_files/` and
`tests/` paths in Aether shouldn't shadow the keyword. The keyword is `test`,
which is currently just an identifier.

---

## 5. String interpolation coverage

Aether has interpolation. Flint's distinguishing feature is *what types it
covers without spec strings*:

```rs
i32x4 v = (-10, -20, -30, -40);
str s = $"v = {v}";          // "v = (-10, -20, -30, -40)"

bool8 b8 = u8(27);
str s = $"b8 = {b8}";        // "b8 = 00011011"

f64x4 v4d = (-1.123, 2.234, -3.345, 4.456);
str s = $"v4d = {v4d}";      // "v4d = (-1.123, 2.234, -3.345, 4.456)"

err err_val = ...;
str s = $"err = {err_val}";  // "err = ErrArithmetic.NullDivision"
```

### What's worth lifting

The pattern is "every primitive type has a canonical string representation
the compiler knows." For Aether this means `aether_string_from_*` for every
new built-in that gets added. Cheap, formulaic, eats real friction.

Specifically: any time a new primitive or a SIMD-shaped tuple (§9) is added,
the `from_*` companion should be added at the same time. This belongs on the
checklist for new primitives in `LLM.md`'s "Invariants to not break" section.

### Codegen-to-C reimplementation note

The interpolation lowering is unchanged: `$"x={x}, y={y}"` lowers to a
sequence of `string_concat(string_concat(string_concat("", "x="), aether_string_from_<type>(x)), …)`.
The work is purely runtime-side: implement `aether_string_from_<T>` for each
new type. Flint's representation choices are reasonable defaults to copy:

- Vectors: `(a, b, c, d)` parens-comma style.
- Bitsets: MSB-first bit string `"00011011"`.
- Errors: `Domain.Variant` (e.g. `"ErrArithmetic.NullDivision"`).

---

## 6. Grouped field access/assign

### Flint surface

```rs
data Vector2i:
    i32 x;
    i32 y;
    Vector2i(x, y);

def swp(mut Vector2i v):
    v.(x, y) = v.(y, x);                 // grouped assign, atomic swap

def get(const Vector2i v) -> (i32, i32):
    return v.(x, y);                     // grouped read

def add(mut Vector2 v):
    f32 sum = v.x + v.y;
    v.(x, y) = v.(x, y) + (sum, sum);    // works arithmetically

// Tuples:
data<i32, f32, str> tuple = (3, 2.2, "hello");
print($"tuple.($0, $1, $2) = {tuple.($0, $1, $2)}\n");
```

### Why it's a fit

Pure sugar. No runtime cost, no new lowering primitive. Eliminates the
intermediate-temp dance for swaps and parallel updates.

### Codegen-to-C reimplementation note

Desugar at parse / analyzer:

- **Read** `v.(a, b, c)` → fresh temp tuple `(v.a, v.b, v.c)`.
- **Assign** `v.(a, b, c) = expr` → evaluate `expr` to fresh tuple `t`, then
  emit `v.a = t.0; v.b = t.1; v.c = t.2;`.
- **Compound assign** `v.(a, b) += rhs` → desugar to `v.(a, b) = v.(a, b) + rhs;`.

Order of evaluation: RHS is fully evaluated *before* any LHS write, so swaps
work correctly without a temp the user has to name.

This depends on Aether having a tuple type that survives codegen. The
`split-accessor pattern via TLS` documented in `LLM.md` is a workaround for
exactly this gap. Fixing tuples-cross-FFI is the real prerequisite.

---

## 7. Switch expressions

### Flint surface

```rs
i32 res = switch me:
    VAL1 -> 1;
    VAL2 -> 2;
    else -> 0;

str s = switch me:
    VAL1 -> str("VAL1");
    VAL2 -> str("VAL2");
    VAL3 -> str("VAL3");
    VAL4 -> str("VAL4");                  // exhaustive on enum, no else needed

// Statement form uses `:` after the arm; expression form uses `->`.
switch me:
    VAL1: val = "1";
    VAL2: val = "2";
    else: val = "other";
```

The `:` vs `->` distinction is the only syntactic marker, same `switch`
keyword does both. Variant arms work the same way (§3).

### Codegen-to-C reimplementation note

```c
// Expression form lowers to GCC statement-expression
int res = ({
    int __r;
    switch (me) {
        case VAL1: __r = 1; break;
        case VAL2: __r = 2; break;
        default:   __r = 0; break;
    }
    __r;
});
```

Or, in strict C99 mode (no GCC extensions): hoist the temp.
```c
int res;
switch (me) {
    case VAL1: res = 1; break;
    case VAL2: res = 2; break;
    default:   res = 0; break;
}
```

Aether's MSVC cross-build matters here, GCC statement-expressions don't
work on MSVC. Default to the hoisted-temp lowering for portability; reserve
the statement-expression form for cases where the target is known to be gcc/clang.

---

## 8. DCMP / entity composition

This is the biggest, most distinctive piece of Flint's design. It is
**probably not worth lifting wholesale**, but worth understanding to know
what design space they're claiming.

### Flint surface

```rs
data Wings:
    u32 size;
    u32 flight_time;
    Wings(size, flight_time);

data Legs:
    u32 count;
    f32 speed;
    f32 height;
    Legs(count, speed, height);

func Fly requires(Wings w):
    def fly():
        print($"flying with {w.size}cm wings\n");

func Run requires(Legs l):
    def run():
        print($"running with {l.count} legs\n");

entity Bird:
    data: Wings, Legs;
    func: Fly, Run;
    Bird(Wings, Legs);

def main():
    b := Bird(Wings(10, 100), Legs(2, 1.5, 0.1));
    b.run();
    b.fly();
```

Three concepts:

- **`data D`**: pure state. No methods. Constructor is `D(field1, field2)`.
- **`func F requires(D d)`**: a *behavior module* parameterized over a piece
  of data. `def` inside a `func` becomes methods that the `func`'s required
  data is implicitly threaded into.
- **`entity E`**: composes one or more `data` and one or more `func` modules.
  Method calls on the entity dispatch into the underlying `func` with the
  entity's `data` slice passed in.

Inheritance: `entity Child extends(Parent p): data; func: …;` child can
extend parent's data + func sets.

### What it's not

- Not OOP, `data` has no methods, only fields. `func` has no fields, only
  methods. Combination is by composition at the `entity`.
- Not ECS, `entity` doesn't iterate over component arrays; you call methods
  on a single entity instance.
- Not interfaces, `func` is a *concrete* method bundle, not a contract.

The pitch: take the data-vs-behavior split that ECS has and the
method-on-receiver dispatch that OOP has, glue them together at composition
time.

### Why to skip lifting it

- Aether's actor model already supplies a "data + behavior + dispatch" story
  (actor = data + receive handlers). DCMP would compete with it, not extend it.
- The lift is large: parser changes, analyzer rules for "func F requires X
  attached to entity that contains X," codegen for vtable-or-direct-dispatch.
- The win is mostly stylistic. Aether doesn't have an OOP gap users are
  asking it to fill.

What *is* worth borrowing piecemeal: the `data D: f1; f2; D(f1, f2);` syntax
where the constructor signature is auto-derived from the field list. Aether
already has plain structs; the auto-constructor sugar is a small parser job.

### If it is ever lifted

Codegen path:
- `data` → C struct.
- `func F requires(D)` → namespace of free functions, each taking `D *self` as
  first arg. The `def fly():` inside expands to `void F_fly(D *self)`.
- `entity E` → C struct containing one of each required `data` (or pointer to
  a parent's). Method dispatch is statically resolved: `b.fly()` →
  `F_fly(&b.wings_data)`. **No vtables needed** because composition is fully
  declared at the entity site, there's no late binding.
- Inheritance flattens: child's struct inlines parent's data + adds its own.
  Method resolution order is parser-determined.

This is very tractable codegen. The cost is in parser/analyzer + the cultural
question of whether Aether wants a second composition story alongside actors.

---

## 9. SIMD & bitset primitives

### Flint surface

```rs
u8x2 v2c = u8x2(1, 2);
u8x4 v4c = u8x4(1, 2, 3, 4);
u8x8 v8c = u8x8(1, 2, 3, 4, 5, 6, 7, 8);
i32x4 v4i = (10, 20, 30, 40);
f32x8 v8f = (10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0);

// Arithmetic works element-wise:
i32x3 sum = (1, 2, 3) + (4, 5, 6);   // (5, 7, 9)
f32x3 scaled = vec3 * 4;             // splats scalar across lanes

// Bitsets:
bool8 b8 = u8(10);
assert(not b8.$0);
assert(b8.$1);
assert(str(b8) == "00001010");
```

### Codegen-to-C reimplementation note

C has portable answers here, use them:

- **gcc/clang**: `typedef int32_t i32x4 __attribute__((vector_size(16)));`
  Element-wise arithmetic, `+ - * /`, just works at the language level.
  Element access via union with array.
- **MSVC**: no equivalent. Either emulate (struct of N scalars + manual
  per-op loops; compiler may auto-vectorize), or use `<intrin.h>` SSE/AVX
  intrinsics for known sizes.
- **Bitsets**: `bool8` is just `uint8_t` with `.$N` lowered to
  `(b8 >> N) & 1`. Trivial.

Cross-platform answer: emulate by default (struct + arithmetic loops), opt
into vector-extension lowering with a CFLAG. CI catches divergence between
the two.

This is a real subproject, not Friday-afternoon work. But the user-facing
ergonomics (game/graphics-style code, parser hot loops, Advent of Code style
problems) are huge. Flint's `pong-3.0` example reads naturally because
`f32x2` is everywhere.

### Where it lands in Aether's invariants

- `i32x4` etc. become new primitive types, with `aether_string_from_i32x4`
  per §5.
- Splat behavior: `vec * scalar` works because the scalar gets broadcast.
  Document this, without it the syntax is misleading.
- Tuples-vs-vectors: `(1, 2, 3)` literal is currently a tuple in Aether;
  it should *also* satisfy `i32x3` when assignment context demands it.
  Same dual-targeting Aether already does for `[a, b, c]` against
  `string[]` vs `*StringSeq` (per `LLM.md`).

---

## Smaller items, bullet-form

These are worth being aware of but aren't standalone sections.

- **`mut` on parameters**: Flint requires `mut` on the parameter at the
  call site too: `increment_by(p, 3)` fails with `"Variable 'p' is marked
  as 'const' and cannot be modified!"` if `p` is declared without `mut`.
  Aether has the convention via convention; making it grammatical is a
  small parser job.

- **`const` data**: `const data Globals: ...;` fields are immutable
  module-level constants. Useful for the asset-table / colors / config
  pattern in the pong example. Aether has no equivalent; closest is
  top-level `let`s.

- **Type aliases**: `type Vector3 f32x3` emits in FIP-generated bindings
  too.

- **Range/slice syntax**: `s[..n]`, `s[i..j]`, `s[i..]`, `s[output.len -
  expected.len..]`. Aether has substring functions; range syntax would be
  cleaner, especially in tests.

- **Enhanced for**: `for (idx, char) in output:` gives index + element.
  Aether has plain index-or-element for loops; this needs both.

- **`str` vs `string`**: Flint uses `str` throughout. Aether uses `string`.
  Cosmetic, but Flint's choice is shorter for fields/locals.

- **`i := expr` walrus**: type-inferring shortcut, common in Flint examples.
  Aether has `let`/`var`; `:=` is shorter still and harmonizes with Go-ish
  surface.

- **`++`/`--` post-increment**: present in Flint (`n--`, `i++`). Aether
  doesn't have these. Tradeoff: they introduce expression/statement
  ambiguity, and Aether has gotten by without them.

- **Annotations as `#name`**: `#test_performance`, `#test_should_fail`. Cheap
  hashtag-syntax for parser-recognized markers. Could also drive
  `#deprecated`, `#noinline`, etc.

- **Compiler diagnostics format**: Flint's diagnostics include a leading
  banner with code (`E0000`), line excerpts with `│` rules, and structured
  per-error multi-line explanations:
  ```
  Parse Error at main.ft:1:1
  └─┬┤E0000│
  1 │ extern def hello();
  ┌─┴─┘
  ├─ No '.fip' directory found
  ├─ To be able to interop with extern code you need the FIP set up
  └─ For further information look at 'https://flint-lang.github.io/...'
  ```
  Visually clean. Aether's diagnostic format is plainer; lifting the
  multi-line "├─ ... └─ ..." structure with explanation + link to docs is a
  pure UX improvement.

- **In-tree LSP**: Flint ships `fls/` (Flint Language Server) in the same
  repo, built alongside `flintc`. Aether has nothing here; the first user to
  ask for editor support gets to write it. The fact that Flint's LSP shares
  parser/analyzer code with the compiler (build.zig builds both) is the
  right architecture and worth replicating whenever Aether gets to it.

---

## What Aether has that Flint doesn't

For completeness, these are the directions where Aether is ahead and where
it shouldn't lose ground while porting features in.

- **Capability discipline**, `--emit=lib` capability-empty + `--with=`
  opt-in is a story Flint has nothing equivalent to. Flint extern code is
  fully trusted. Don't compromise this when adding FIP-shaped ingest.
- **`hide`/`seal except`**, per-block lexical capability denial. No
  equivalent in Flint.
- **`libaether_sandbox.so`** LD_PRELOAD libc-call gate, runtime sandbox at
  the syscall layer. Flint has nothing here.
- **In-process embedded interpreters** with `contrib.host.<lang>.run_sandboxed(perms, code)`
  for Lua/Python/Perl/Ruby/Tcl/JS. Flint has no equivalent, and the
  direction is genuinely opposite, not just absent:

  | | Aether `contrib.host.<lang>` | Flint FIP |
  |---|---|---|
  | Direction | Guest interpreter runs *inside* the Aether process | Compiled foreign code is *called from* Flint |
  | Trust model | Guest is untrusted; runs under capability grant + LD_PRELOAD libc gate | Foreign code is fully trusted, native, no containment |
  | What's auto-generated | Nothing, interpreter is embedded as a library | `.fip/generated/<tag>.ft` Flint stubs from foreign headers |
  | Process boundary | In-process | In-process call, but binding generation is out-of-process per-language FIP module |
  | Use case | Embed a scripting layer in an Aether host (plugin systems, untrusted user code) | Use existing C/C++/Java/etc. libraries from Flint code |

  They share surface vocabulary ("multiple languages") but solve opposite
  problems. Don't conflate them when speccing Aether's FIP-shaped ingest
  (§2), that's strictly the *Flint direction*, and it should sit alongside
  `contrib.host.*`, not replace any of it. The capability gate also
  applies cleanly: `use Fip.c` gets banned under `--emit=lib` by default
  for the same reason `std.fs` is, and `contrib.host.*` continues to gate
  guests via the existing `perms` list.
- **Actor model**, `send`/`receive` with declared message types. Flint has
  no concurrency primitive visible in the language surface.
- **C as the lowering target**, portable, debuggable, every platform's
  toolchain is a target. Flint locks in LLVM 21 and ships its own `lld`
  copy (vendored as a git submodule, ~hours to bootstrap from scratch).
  Aether can build with vanilla `cc` on a fresh box. Don't lose this.
- **CHANGELOG-driven release flow**, Flint uses a similar `[current]` →
  `[X.Y.Z]` rename, but the CHANGELOG isn't load-bearing for the way
  Aether's binaries identify their feature set.
- **`std.json` insertion-order contract**, Aether's `std.json` documents
  insertion order across parse and builder paths; downstream relies on it.
  Flint uses `json-mini` (vendored) but doesn't (visibly) commit to this.
- **`std.string.from_int(int)` / `string.from_long(long)`** ABI split for
  MSVC `long`-is-32-bit, Aether's invariant. Flint targets only Linux +
  Windows-MinGW-w64, so MSVC isn't on their map.

---

## Implementation order

If the high-payoff pieces were to be lifted, in priority order:

1. **Optionals** (§1). Two weeks of parser + codegen + runtime work. Single
   biggest ergonomic delta per line of compiler code touched.
2. **Variant types** (§3). Similar shape; benefits from optional-extract `?(T)`
   so do them after §1.
3. **`test "name":` blocks** (§4). One week. Removes a category of
   `tests/regression/` pain.
4. **Switch expressions** (§7). Couple of days; touches parser + codegen
   only.
5. **Grouped field access/assign** (§6). Couple of days; pure desugaring.
6. **String interpolation coverage** (§5). Continuous; add `from_<T>` per
   primitive incrementally.
7. **FIP-shaped C interop** (§2). Largest project. Probably a month for an
   `aether-bindgen` MVP that covers C99 + GCC attributes that real libraries
   use, plus the `.fip/` cache integration in `aetherc`. Strongly worth it
   because of how much downstream `extern` boilerplate it deletes.
8. **SIMD primitives** (§9). Defer. Real subproject. Probably tied to a
   downstream user actually asking for `f32x4`.
9. **DCMP** (§8). Probably skip permanently. Flint claims this as their
   distinguishing feature; Aether's distinguishing feature is the
   capability story. Different claims.

Most attractive single import is **Optionals + Variants together**, they
cooperate (optional-extract returns a variant arm wrapped in `T?`), they share
codegen machinery (both are tagged-union lowerings), and they cover the
biggest gap in Aether's type system as it stands.

---

## TL;DR for triage

The full doc above is a survey of Flint, the [flint-lang/flintc](https://github.com/flint-lang/flintc) project on GitHub; not to be confused with the older static-analyzer Flint. Like the Flux and Fir comparisons, it's written to let Aether decide deliberately what to absorb.

Flint is a Python-ish (indentation + `;`) systems language compiling to LLVM IR with embedded `lld`. Distinguishers vs Aether: first-class optionals (`T?`), variant/tagged-union types, FIP (auto-generated C bindings from headers), built-in test syntax, SIMD primitives, and DCMP (entity composition).

**The doc's own implementation order (Section 11):**

1. **Optionals (`T?`, `?.`, `!`, `??`)**, single biggest ergonomic delta per line of compiler code. Filed as separate issue, see cross-refs.
2. **Variant types**, share codegen with optionals (both tagged-union lowerings); attractive to bundle.
3. **`test "name":` blocks**, removes per-file regression-harness friction.
4. **Switch expressions**, collapses temp-var dance.
5. **Grouped field access/assign** (`v.(x, y) = v.(y, x)`), pure desugaring.
6. **String interpolation coverage**, incremental `from_<T>` per primitive.
7. **FIP-shaped C interop**, largest project; replaces a lot of `extern fs_foo_raw` boilerplate plus the split-accessor TLS pattern documented in LLM.md. Worth a month-scale investment because of how much it deletes.
8. **SIMD primitives**, defer until a downstream user asks.
9. **DCMP / entity composition**, explicit skip; competes with actor model rather than complementing it.

**The single most attractive landing per the doc:** Optionals + Variants together (§1 + §3). They cooperate (`?(T)` extract returns an optional), share tagged-union codegen, and address the biggest gap in Aether's type system. Optionals is filed as a focused issue; Variants would be a natural follow-up issue once Optionals lands.

## Note on overlap with Flux and Fir comparisons

The three comparison docs converge on a few themes:

- **Sum types / variants**: Fir (row-typed open variants), Flint (tagged), Flux (deferred). All three flag the gap; the question is which shape.
- **Pattern matching exhaustiveness**: Fir and Flint both flag this; Flux doesn't.
- **String interpolation type coverage**: Flint and Flux both have this; Fir uses backticks. Aether already has interpolation; the question is whether to broaden the auto-formatted type set.
- **Bit-precise types vs. SIMD**: Flux pulls hard on `data{N}`; Flint pulls on `i32x4`/`f32x8`. Both target hot-path / binary-format code but at different abstraction levels.

The biggest divergence: **Flint's FIP** has no analogue in the other comparisons. Auto-generating Aether `extern` decls + matching `data` types from a C header would replace a category of friction (split-accessor TLS pattern, length-aware `_get_*_length` accessors, hand-rolled bindings for vendored C libs). That's potentially worth its own focused issue separate from this umbrella.

## What this issue is NOT asking

- Not a roadmap. "Use this as a menu, not a roadmap."
- Not asking for any single feature to ship as-listed.
- Not litigating §8 (DCMP / entity composition). Correctly identified as a competing-with-actors design that doesn't fit Aether's identity.
- Not litigating §7 indentation-sensitive syntax (skipped per the broader convention across all three comparisons).

## Cross-refs

- Optionals (§1): filed as separate issue
- Variants (§3): not filed, flag for separate issue if/when Optionals lands (they share codegen)
- FIP (§2): not filed, flag for separate issue if you want it pursued separately from this umbrella
- Source project: Flint's repo: [flint-lang/flintc](https://github.com/flint-lang/flintc).
- Sister surveys: Flux, Fir
- Concrete `extern` boilerplate FIP would target: any of the `*_raw` + `*_get_*` + `*_get_*_length` + `*_release_*` quartets in std/* (e.g. `fs_try_read_binary` family before the tuple unification)
