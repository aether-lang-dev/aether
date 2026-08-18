# Aether Language Reference

Complete syntax and semantics of the Aether programming language.

## Overview

Aether is a statically-typed compiled language with actor-based concurrency and
type inference. It compiles to C.

## Paradigm placement

Aether is a **hybrid that leans closer to functional than object-oriented**, sitting next to Go and Rust in the middle of the paradigm spectrum:

```
   pure FP                    OO + FP hybrid                  pure OO
   Haskell                    Scala  Rust  Go                 Smalltalk
   |--------|--------|--------|--------|--------|--------|--------|
                              ↑
                              Aether
```

The placement is deliberate, Aether picks the pieces of each tradition that compose cleanly and skips the ones (deep inheritance hierarchies, monad transformers, etc.) that don't earn their cost in a compiles-to-C language. Concretely:

### Where Aether leans functional

- **Closures + trailing blocks are first-class** and are *the* idiomatic control-flow construction. The "DSL with scope" pattern (`window("Demo") { vg { circle(...) { fill("#F00") } } }`) is built from closures running in the caller's lexical scope, not from method dispatch on a builder object. See [Closures and Builder DSL](closures-and-builder-dsl.md).
- **No classes, no inheritance, no `self` / `this`.** Structs are plain data records (`struct Circle { cx: float, cy: float, r: float }`) with no methods attached. Behaviour comes from free functions taking the struct as an argument: `circle_area(c)` not `c.area()`.
- **Type-annotated parameters with full inference** and **multi-value returns** (`-> (long, string)` for value-and-error tuples). Failure is carried in the return value rather than thrown, so the happy path has no exception machinery in it.
- **Modules with explicit `exports`**, namespaced free functions, not classes-as-namespaces. The visible surface of a module is the export list, not a public-method set.
- **`fn` as a first-class type**, passed, stored, boxed, unboxed. Aether's closures lower to `_AeClosure { void(*fn)(void); void* env; }` with structural sharing through ref cells (`ref()` / `ref_get` / `ref_set`); the runtime treats them as values, not as method receivers.
- **No method-dispatch machinery in the compiler.** There's no vtable, no virtual call. Every function call resolves to a direct C function call. The bare-fn-to-closure adapter machinery and the `fn ↔ ptr` coercion rules exist to make first-class function values work end-to-end, a strictly functional priority.

### Where Aether borrows from OO (in a limited way)

- **Actors** with state and message-receive handlers are the one piece of OO-shape machinery present, encapsulated state behind a message boundary. This is the Erlang / Pony actor model rather than Smalltalk-OO: no polymorphism, no inheritance, no method overriding. An actor is a stateful object reachable only through messages.
- **The `_ctx` / builder-context stack DSL** lets you write code that *reads* like a fluent OO method chain (`panel("Settings") { button("OK") }`), but the underlying mechanism is closure scope + an implicit context argument, not method dispatch. The compiler injects `_ctx` automatically; no receiver is involved.
- **Receiver-namespace fallback** for trailing blocks (`bash.test(b) { script("...") }` resolves `script` as `bash_script`) makes free-function calls look OO at the call site, but it's a name-resolution rule, not method dispatch.
- **Struct-field assignment and member access** (`h.cb = c`, `c.r`) are present, but they're data-oriented (record reads/writes), the same shape Go and C have without claiming OO.

### Where Aether is neither, it's just systems-pragmatic

- Manual heap discipline is visible, `box_closure` / `unbox_closure`, `ref()` cells, the automatic heap-string ownership tracker (`_heap_<name>` companion flags), `defer` for resource cleanup. Closer to Rust's explicit ownership story than to either paradigm's purist position.
- Aether's closure model deliberately trades Lisp/Smalltalk-style runtime-owned environments for generated C structs whose lifetimes are mostly inferred by the compiler. See [Closure lineage and runtime tradeoffs](design/closure-lineage-and-runtime-tradeoffs.md) for the comparison.
- The C ABI / `extern` boundary is a first-class concern, not a hidden detail. The typechecker pragmatically allows `int ↔ ptr` and `fn ↔ ptr` coercions because surface ergonomics matter more than type-purity at the FFI boundary.
- The capability + sandbox system (compile-time module gate / scope-level `hide` / `seal except` / LD_PRELOAD libc gate) is its own concern, orthogonal to both paradigms.

### What this means in practice

If you've written Go, you'll recognise the shape immediately: free functions, structs, channels (`!` send, `?` ask), explicit error returns, no classes. If you've written Elixir, the actor model and trailing-block DSLs will feel familiar. If you've written Ruby or Groovy, the "DSL with scope" idiom is borrowed directly from your lineage (Matz's term for it). If you're coming from Java or Python, the biggest adjustment is the absence of method dispatch (`circle_area(c)` instead of `c.area()`); the rest of the surface is approachable.

Aether is not, and has no plans to be, a pure FP language (no Hindley-Milner inference, no purity tracking, no monadic effects). It's also not, and has no plans to be, a class-based OO language (no inheritance, no virtual dispatch, no `self`). It picks the cross-paradigm subset that survives compilation to readable C and stays useful for systems work, concurrent servers, language ports, build tooling, embedded scripting.

## Table of Contents

1. [Types](#types)
2. [Variables](#variables)
3. [Functions](#functions)
4. [Pattern Matching Functions](#pattern-matching-functions)
5. [Control Flow](#control-flow)
6. [Match Statements](#match-statements)
7. [Switch Statements](#switch-statements)
8. [Defer Statement](#defer-statement)
9. [Memory Management](#memory-management)
10. [Optionals](#optionals)
11. [Structs](#structs)
12. [Messages](#messages)
13. [Actors](#actors)
14. [Operators](#operators)
15. [Modules and Imports](#modules-and-imports)
16. [Extern Functions](#extern-functions)
17. [Built-in Functions](#built-in-functions)
18. [Comments](#comments)
19. [Compilation](#compilation)

---

## Types

### Primitive Types

| Type | Description | Example |
|------|-------------|---------|
| `int` | 32-bit signed integer | `42`, `-17`, `0xFF`, `0b1010` |
| `float` | 64-bit floating point | `3.14`, `-0.5` |
| `string` | UTF-8 encoded strings | `"Hello"` |
| `bool` | Boolean type | `true`, `false` |
| `byte` | Unsigned 8-bit (0..255) | `byte b = 0xFF` |
| `void`¹ | No value (for functions) | `extern free(p: ptr) -> void` |
| `long` | 64-bit signed integer | `long x = 0` |
| `longdouble` | C `long double` widest float (C interop) | `extern strtold(...) -> longdouble` |
| `ptr` | Raw pointer (for C interop) | `null` |

¹ `void` is **not** a reserved word, there is no `void` token; it is a plain identifier used by convention to spell the absence of a return value. A function that omits its `-> Type` annotation is the canonical void declaration (see [Functions](#functions)). It is never a value type and cannot be used for a variable or field.

#### `byte` unsigned 8-bit

The `byte` type maps to `unsigned char` in C. Use it for type-precision in struct fields, function parameters, returns, and locals where a value is genuinely an octet, packed-tag bytes (`flags & 0x80`), opcode discriminators, NaN-boxing pointer tags, network protocol headers, on-disk file format fields. For *bulk* byte storage (binary buffers, byte arrays), reach for `std.bytes` (the mutable byte buffer) instead.

```aether
struct OpCode {
    byte op
    byte flags
    int  imm
}

set_tag(t: byte) -> byte {
    return t & 0x7F     // bitwise byte op byte → byte
}

main() {
    byte b = 0xA5
    byte high = b & 0xF0    // stays byte
    n = b + 1               // byte + int → int (promotes)
}
```

**Range check on integer literals.** Assigning an out-of-range integer literal to a `byte` slot is a compile-time error: `byte b = 256` is rejected. Non-literal int → byte assignments compile and truncate at runtime (`byte b = some_int` keeps the low 8 bits), matching how other narrowings (`int64 → int`) behave.

**Arithmetic.** `byte op byte → byte`; mixed `byte op int → int` (the wider type wins). This keeps NaN-boxing / packed-tag patterns expressible (`tag & 0x07` stays a byte) while letting general arithmetic widen naturally.

#### `longdouble` C `long double`

The `longdouble` type maps to C `long double`, the widest floating type, for exact-decimal numeric paths a C interop layer expects it (libc `strtold`, `INCRBYFLOAT`-style score conversion). It supports arithmetic (`+ - * /`), comparison, and conversion to/from `int` and `float`; as the widest numeric it wins promotion (`longdouble op int` / `longdouble op float` → `longdouble`). It's usable in locals, function params/returns, struct fields, and `extern` signatures. There is no `longdouble` literal, values arrive from an extern (`extern strtold(s: cstring_const, end: ptr) -> longdouble`) or by widening an `int`/`float`. Interpolation and `print` format it with `%Lg`/`%Lf`.

```aether
extern strtold(s: cstring_const, end: ptr) -> longdouble
incr(cur: longdouble, by: longdouble) -> longdouble { return cur + by }

main() {
    base = strtold("3.14159265358979323846", null)
    next = incr(base, base)        // long double arithmetic, full precision
    println("v=${next}")           // %Lg
}
```

**C-side mapping.** `byte` lowers to `unsigned char` in the generated C, not `uint8_t`. The two are typedef-compatible on most platforms but C's strict-aliasing rules give `unsigned char *` an exemption (it can alias *any* type for read/write); `uint8_t *` does not. Since `byte` is exactly the type used to inspect the bytes of other types' storage, `unsigned char` is the right choice.

### Composite Types

| Type | Description |
|------|-------------|
| `struct` | User-defined composite data |
| `actor` | Concurrency primitive with state |
| `message` | Structured data for actor communication |
| `array` | Fixed-size homogeneous collections |

### Array Types

```aether,fragment
int[10] numbers;           // Array of 10 integers
string[5] names;           // Array of 5 strings
float[100] values;         // Array of 100 floats
```

**Array-to-pointer decay.** A named fixed-size array decays to a pointer to its first element in pointer context, when assigned to an inferred binding, passed as a `ptr`-typed argument, or compared against a pointer (the same rule C uses):

```aether,fragment
byte[128] static_ids
ids = static_ids           // `ids` is inferred as a `ptr` (decays), not an array
heap = null
if id_count > 8 {
    heap = malloc(256)
    ids = heap             // legal: `ids` is a reassignable pointer
}
```

This is the stack-buffer-with-heap-fallback idiom (`T buf[N]; T* p = buf; if (n > N) p = malloc(...)`). Only a *named array* decays; an array *literal* initializer (`x = [1, 2, 3]`) still binds a real array. To keep the array type, annotate the binding explicitly (`x: byte[128] = ...`).

### Sequence Types (`*StringSeq`)

`*StringSeq` is a cons-cell linked list of strings, Erlang/Elixir-shaped, with O(1) head/tail/cons/length and refcount-based structural sharing. Empty list is the `NULL` pointer; each cell carries a cached length.

```aether
import std.string

main() {
    s = string.seq_empty()
    s = string.seq_cons("c", s)
    s = string.seq_cons("b", s)
    s = string.seq_cons("a", s)        // s = a -> b -> c
    println(string.seq_length(s))       // 3 (O(1))

    // Pattern-match destructure works directly:
    match s {
        []      -> { /* end */ }
        [h | t] -> { println(h); /* h: string, t: *StringSeq */ }
    }

    string.seq_free(s)
}
```

The full surface lives in `std.string` (alongside `string.array_*` for the legacy `AetherStringArray` shape). See [sequences.md](sequences.md) for the worked examples and the literal-disambiguation rule (`[a, b, c]` builds a cons chain when the target type is `*StringSeq`, vs a static C array when the target is `string[]`).

### Numeric Literal Formats

Integer literals support hex, octal, and binary notation. Underscore separators are allowed anywhere in digits for readability.

| Format | Prefix | Example | Value |
|--------|--------|---------|-------|
| Decimal | (none) | `255` | 255 |
| Hexadecimal | `0x` / `0X` | `0xFF` | 255 |
| Octal | `0o` / `0O` | `0o377` | 255 |
| Binary | `0b` / `0B` | `0b1111_1111` | 255 |

```aether,fragment
mask = 0xFF
flags = 0b1010_0101
perms = 0o755
big = 1_000_000
```

All numeric literal formats work with bitwise operators and in any expression context.

---

## Variables

Variables support both explicit types and automatic type inference:

```aether,fragment
// Type inference (recommended), bare assignment is the canonical form
x = 10
y = 20
name = "Alice"
pi = 3.14159

// Explicit types (optional)
int z = 30
string greeting = "Hello"
float temperature = 98.6
```

Variables are inferred from their initialization or usage context.

**`let` / `var` are optional keywords**, accepted but not required. Bare Python-style assignment is the canonical form and is what most stdlib code uses. `let x = 10` and `var x = 10` parse identically to `x = 10`. There is no semantic distinction between `let` and `var` Aether is not Rust; mutability is a property of the binding's later use, not its declaration.

**Semicolons are optional.** Aether parses end-of-line as a statement terminator. The samples above use no semicolons; older samples in this doc may show `;` for clarity. Either is fine.

### Casting between types

Aether has **no general-purpose cast operator**. The `as` keyword is reserved for module aliasing and three specific value-cast forms:
- `import mod as alias` module aliasing (only inside `import` statements)
- `expr as *StructName` pointer-overlay struct cast (a leading `*` then a struct name; see [§ Pointer-to-struct type](#pointer-to-struct-type-structname-and-expr-as-structname) below)
- `expr as fn(T1, T2, ...) -> R` function-pointer cast (call a stored pointer with type checking; see [§ Function-pointer parameters](#function-pointer-parameters-fnt1-t2---r) below)
- `expr as T[]` typed-array view cast (reinterpret a raw pointer as a `T[]`)

After `as` the parser also accepts a primitive **value cast**: `n as int` and other numeric casts compile and run, and a cast between a distinct type and its base type is allowed too. Casts the type system can't justify (for example `buf as string` or `p as ptr`) still parse, but are rejected at type-check with `E0200`, use a named helper for those. The `*StructName`, `fn(...) -> R`, and `T[]` forms remain available. Other primitive conversions:

| From → To | How |
|---|---|
| `int` → `ptr` (e.g. passing an int to a `ptr`-typed extern param) | Implicit. The compiler emits `(void*)(intptr_t)` automatically at the call site. |
| `string` → C `const char*` (passing a `string` to a C extern) | Implicit. The auto-unwrap injects `aether_string_data(arg)` at call sites for `string`-typed extern parameters. |
| `int` → `string` | `string.from_int(n)` (and `string.from_long(n)`, `string.from_float(f)`). |
| C `const char*` → Aether `string` | Assignment to a `string`-typed variable, or returning from a function declared `-> string`. The returned `ptr` is treated as a borrowed C-string until it crosses into refcounted-string territory. |
| `int` ↔ `int64` / `byte` ↔ `int` | Implicit safe widenings (`byte → int`, `int → int64`). The narrowing direction (`int → byte`) requires a literal-range check and truncates non-literals at runtime. See [§ `byte`](#byte-unsigned-8-bit). |
| Reinterpret a raw `ptr` as a typed struct view, function pointer, or array | `expr as *StructName`, `expr as fn(...) -> R`, or `expr as T[]` (the only `as` forms for values). |

### Null

The `null` keyword represents a null pointer, typed as `ptr`:

```aether,fragment
x = null             // inferred: ptr
if x == null {
    println("no value")
}
```

### Constants

Top-level constants are declared with `const`:

```aether
const MAX_SIZE = 100
const GREETING = "hello"
const PI = 3

main() {
    println(MAX_SIZE)           // 100
    half = MAX_SIZE / 2         // constants work in expressions
}
```

Constants are emitted as `#define` in generated C, zero runtime cost.

#### Module-level constant arrays, lookup tables

A `const` array lowers to a file-scope `static const` table, allocated once and shared across every call (not re-initialised per call), the right shape for a table-driven algorithm:

```aether,fragment
const PRIMES[] = [2, 3, 5, 7, 11]          // element type inferred (int)
const CRC16TAB: uint16[256] = [ 0x0000, 0x1021, /* ... */ ]   // element type pinned
```

- `const NAME[] = [...]` infers the element type from the literals (`int` for integer literals).
- `const NAME: T[N] = [...]` pins the C element type, `T` may be `uint8` / `uint16` / `uint32` / `uint64` / `int` / `long` so e.g. a CRC16 table emits `static const uint16_t NAME[256]` rather than a 4×-wider `int[]`, and matches a C header that expects the packed type. Integer literals narrow to the chosen element type (the explicit, compile-time-constant intent, like `byte b = 5`). Indexed access (`NAME[i]`) emits `NAME[i]`; the array is read-only.

**The RHS of `const` must be a compile-time constant expression.** Allowed forms: literals (int / float / bool / string / null), other consts referenced by name, unary / binary expressions over those, and string interpolation where every interpolated value is itself const. **Function calls are rejected** at typecheck time:

```aether,fragment
const G_BUF = malloc(64)        // ERROR: const initializer must be a
                                 // compile-time constant expression
```

The reason is `const`'s substitution-at-each-use semantics: the compiler inlines the RHS at every reference rather than storing the value. For literal RHSs this is fine; for `make_thing()` it would re-call the function on every reference, allocating fresh state. If you need a process-global heap object initialised once and read everywhere, use [`std.config`](stdlib-reference.md#stdconfig-stringstring-kv) for string state or [`std.actors`](stdlib-reference.md#stdactors-name--actor_ref-registry) for actor references, both are described in the stdlib reference.

**Exception, `sizeof` / `offsetof`.** The two layout builtins `sizeof(T)` and `offsetof(T, field)` *are* allowed in a `const` initializer (and arithmetic over them), even though they are spelled with call syntax. They lower to C compile-time constant expressions (`(int)sizeof(struct T)` / `(int)offsetof(struct T, field)`), no evaluation, allocation, or side effect, so the substitution-at-each-use semantics are harmless:

```aether,fragment
extern struct JSString { gc_mark: uint64 : 1  len: uint64 : 60  buf: byte[] }

const SIZEOF_JSSTRING = sizeof(JSString)          // ok, folds to a C constant
const STR_BUF_OFFSET  = offsetof(JSString, buf)   // ok
const STR_PADDED      = sizeof(JSString) + 8      // arithmetic over them is ok too
```

This lets a port that mirrors C structs as `extern struct` overlays centralise its offset/size table as named consts that are **self-verifying by construction**, the C compiler computes each value, so it cannot drift from the real layout, instead of hand-maintaining numeric offsets plus `_Static_assert` drift guards. The general "no function calls in a `const` initializer" rule is unchanged; these two layout builtins are the only carve-out.

### Mutable module-level globals

Where a `const` is a fixed value, a module-level `var` is **one persistent, mutable word of state** shared by every function in the module, a PRNG seed, a monotonic counter, a one-slot cache:

```aether,fragment
var rand48_x: uint64 = 0x1234ABCD330E      // module scope, mutable

next_rand() -> uint64 {
    rand48_x = (rand48_x * 0x5DEECE66D + 0xB) & 0xFFFFFFFFFFFF
    return (rand48_x >> 17) & 0x7FFFFFFF
}
```

- The type annotation is optional: `var hits = 0` infers `int` from the initializer, exactly like a local.
- A `var` lowers to a **file-scope `static`** in the generated C. Reads and writes from same-module functions are plain identifier access, no accessors, no indirection. A `name = expr` statement whose name is a module `var` always writes that global (the name is in scope everywhere in the module); pick a different name for a function-local.
- The **initializer must be a compile-time constant expression**, the same restriction as `const` (and for the same underlying reason, C requires a `static`'s initializer to be constant). Initialize to a constant and compute the live value from a function at startup if you need more.
- Globals are **module-private** and **non-atomic**, matching the C statics they replace. They are not shared across modules and carry no built-in synchronization; guard concurrent access yourself (or keep such state on an [actor](actor-concurrency.md)).

---

## Functions

Aether functions have **two canonical declaration forms**, pick by whether you want explicit types:

```aether,fragment
// 1. Inferred (no type annotations), the most compact form
add(a, b) {
    return a + b
}

greet(name) {
    println("Hello, ${name}")
}

// 2. Explicit types, the recommended form for stdlib / public APIs.
//    Parameter types after `:`, return type after `->`.
add(a: int, b: int) -> int {
    return a + b
}

greet(name: string) -> string {
    return "Hello, ${name}"
}
```

Both forms parse cleanly. The `name(params) -> ReturnType { … }` shape is what every stdlib module uses (`std/string/module.ae`, `std/bytes/module.ae`, …), match that style for new code.

**Form to avoid: C-style prefix `int add(int a, int b) { … }`.** This form parses for backwards compatibility but is not the recommended style and isn't used in any stdlib module. New code should use the arrow form (`add(a: int, b: int) -> int { … }`) instead.

**Void functions** simply omit the `-> Type` annotation:

```aether,fragment
print_hello() {
    println("Hello")
}
```

There is no `void` keyword in the return-type position, a missing return-type annotation IS the void declaration. The `main()` function is the entry point and is always void.

**`main()` takes no parameters.** There is no Aether-side `main(argc, argv)` form, the signature is uniform across every program. The runtime stashes the C-side `argc` / `argv` at startup (the codegen wraps your zero-arg `main()` in a C `int main(int argc, char** argv)` that calls `aether_args_init(argc, argv)` before any user code runs), and Aether code reaches command-line arguments through `std.os`:

```aether
import std.os

main() {
    n = os.aether_args_count()
    i = 0
    while i < n {
        println("argv[${i}] = ${os.aether_args_get(i)}")
        i = i + 1
    }
}
```

`os.argv0()` returns argv[0] as a string (empty if uninitialised). `os.aether_argv_raw()` exposes the original `char**` for C-interop callers that need to forward it unchanged. Shorter ergonomic spellings exist: `os.args_count()` and `os.args_get(i)` (the latter returns an owned copy, `""` when out of range). See [Standard Library Reference § `std.os`](stdlib-reference.md) for the full surface.

### Default arguments

Parameters can carry a default expression:

```aether,fragment
greet(name: string, greeting: string = "Hello") -> string {
    return "${greeting}, ${name}!"
}

greet("Ada")        // -> "Hello, Ada!"
greet("Ada", "Hi")  // -> "Hi, Ada!"
```

Rules:
- By convention, defaults should trail required parameters (the Python
  shape): once a default appears, every subsequent parameter should
  also have one. This is a style convention, not a compiler-enforced
  rule, the typechecker does not currently reject a default that is
  followed by a required parameter, but positional calls to such a
  function are awkward, so don't write them.
- The default expression is evaluated at the **call site**, not the
  declaration site. Default expressions cannot reference other
  parameters of the same function (they're typechecked in the
  caller's scope where parameter values aren't visible).
- The default-fill happens at typecheck time, codegen sees a
  fully-populated call.

### Source-location intrinsics

Three globally-visible identifiers expand at codegen time:

| Intrinsic | Type | Substitutes to |
|---|---|---|
| `__LINE__` | `int` | The literal source line of the AST node |
| `__FILE__` | `string` | The source-file path |
| `__func__` | `string` | The enclosing C/Aether function name |

Used at an explicit call site, the values reflect that site:

```aether
my_log(msg: string, line: int, file: string, fn: string) {
    println("[${file}:${line} ${fn}] ${msg}")
}

main() {
    my_log("hello", __LINE__, __FILE__, __func__)
    // -> [/path/main.ae:6 main] hello
}
```

Used as **default values**, they substitute the caller's location,
the killer ergonomic for logging / assertion frameworks:

```aether
my_log(msg: string,
       line: int    = __LINE__,
       file: string = __FILE__,
       fn: string   = __func__) {
    println("[${file}:${line} ${fn}] ${msg}")
}

main() {
    my_log("compact form")
    // -> [/path/main.ae:9 main] compact form
}
```

The substitution happens at typecheck time: the typechecker clones
the default expression into the call's argument list and rewrites
any embedded `__LINE__` / `__FILE__` / `__func__` to use the call
site's metadata.

---

## Pattern Matching Functions

A function can be written as several clauses, each matching a different shape of
argument, with guards to narrow a clause further:

### Basic Pattern Matching

```aether,fragment
// Match on literal values
factorial(0) -> 1
factorial(n) -> n * factorial(n - 1)

// Fibonacci with multiple clauses
fib(0) -> 0
fib(1) -> 1
fib(n) -> fib(n - 1) + fib(n - 2)
```

### Guard Clauses

Guards add conditions using the `when` keyword:

```aether,fragment
// Classify numbers using guards
classify(x) when x < 0 -> print("negative\n")
classify(x) when x == 0 -> print("zero\n")
classify(x) when x > 0 -> print("positive\n")

// Factorial with guard
factorial(0) -> 1
factorial(n) when n > 0 -> n * factorial(n - 1)

// Grade calculation with multiple ranges
grade(score) when score >= 90 -> "A"
grade(score) when score >= 80 -> "B"
grade(score) when score >= 70 -> "C"
grade(score) when score >= 60 -> "D"
grade(score) when score < 60 -> "F"
```

### Multi-Statement Arrow Bodies

Arrow functions can have block bodies with `-> { ... }`. The last expression is the implicit return value:

```aether,fragment
// Single expression (existing)
twice(x) -> x * 2

// Multi-statement with implicit return
sum_squares(a, b) -> {
    sq_a = a * a
    sq_b = b * b
    sq_a + sq_b
}

// Multi-statement with early return
clamp(x, lo, hi) -> {
    if x < lo {
        return lo
    }
    if x > hi {
        return hi
    }
    x
}
```

This allows complex logic in arrow-style functions without switching to block syntax.

### Multi-parameter Guards

```aether,fragment
// Max of two numbers
max(a, b) when a >= b -> a
max(a, b) when a < b -> b

// GCD with pattern matching
gcd(a, 0) -> a
gcd(a, b) when b > 0 -> gcd(b, a - (a / b) * b)
```

### Mutual Recursion with Guards

```aether,fragment
is_even(n) when n == 0 -> 1
is_even(n) when n > 0 -> is_odd(n - 1)

is_odd(n) when n == 0 -> 0
is_odd(n) when n > 0 -> is_even(n - 1)
```

---

## Control Flow

### If Statements

```aether,fragment
if (x > 0) {
    print("Positive\n");
} else if (x < 0) {
    print("Negative\n");
} else {
    print("Zero\n");
}
```

### If-Expressions

`if`/`else` can be used as an expression that produces a value (like a ternary operator):

```aether,fragment
// Assign based on condition
max = if a > b { a } else { b }

// Use inline in function calls
println(if x > 0 { x } else { 0 - x })

// Nested if-expressions
grade = if score >= 90 { 4 } else { if score >= 80 { 3 } else { 2 } }
```

Both branches must produce a value of the same type. The `else` branch is required.

### While Loops

```aether,fragment
i = 0;
while (i < 10) {
    print(i);
    print("\n");
    i = i + 1;
}
```

### For Loops

```aether,fragment
for (i = 0; i < 10; i = i + 1) {
    print(i);
    print("\n");
}
```

### Range-Based For Loops

Iterate over a range with `for VAR in START..END`:

```aether,fragment
// Prints 0 1 2 3 4
for i in 0..5 {
    print(i)
    print(" ")
}

// Sum with variable bound
sum = 0
for i in 1..n {
    sum += i
}
```

The range `start..end` is exclusive of `end` (like Python's `range(start, end)`). It desugars to a C-style for loop internally.

### Loop Control

```aether,fragment
// Break - exit loop early
for (i = 0; i < 100; i = i + 1) {
    if (i == 50) {
        break;
    }
}

// Continue - skip to next iteration
for (i = 0; i < 10; i = i + 1) {
    if (i == 5) {
        continue;
    }
    print(i);
}
```

#### Labeled break / continue

A `while` or `for` loop may carry a label (`label: while ...`), and `break label` / `continue label` then target that loop from inside a nested loop, `break label` exits the labeled loop, `continue label` jumps to its next iteration. This replaces the boolean-flag dance a C port would otherwise need for a nested-loop early exit (the C `goto cleanup` / labeled-break idiom):

```aether,fragment
outer: while have_more() {
    while scan_row() {
        if found_match() {
            break outer        // exit BOTH loops
        }
        if skip_row() {
            continue outer     // stop this row, advance the outer loop
        }
    }
}
```

- The label sits before the loop keyword (`outer: while`, `scan: for`). It must be on the same line as the `break` / `continue` that references it.
- A `break label` / `continue label` whose label names no enclosing loop is a compile-time error.
- Defers in the loop scopes a labeled break/continue unwinds still run (in LIFO order) before the jump, the same cleanup guarantee as an ordinary scope exit.
- Unlabeled `break` / `continue` are unchanged and act on the innermost loop.

---

## Match Statements

Match statements provide pattern-based dispatch:

### Integer Matching

```aether,fragment
match (value) {
    0 -> { print("zero\n"); }
    1 -> { print("one\n"); }
    2 -> { print("two\n"); }
    _ -> { print("other\n"); }
}
```

### String Matching

Strings are compared by content (via `strcmp`), so string literal arms work correctly:

```aether,fragment
match (command) {
    "start" -> { println("starting...") }
    "stop" -> { println("stopping...") }
    "help" -> { println("available: start, stop, help") }
    _ -> { println("unknown command") }
}
```

### Ranged and multi-value cases

A `match` arm (and a `switch` case) can match a **range** of ordinal values or a
**comma-separated list** of values and ranges, instead of a single literal:

```aether,fragment
grade = match score {
    90..=100   -> "A"     // ..= inclusive:  90 through 100
    80..<90    -> "B"     // ..< half-open:  80 through 89 (excludes 90)
    70..<80    -> "C"
    60, 61, 62 -> "D"     // comma-list of values
    _          -> "F"
}
```

- `..=` is an **inclusive** range (both ends match); `..<` is **half-open**
  (the high end is excluded), consistent with the exclusive `for i in 0..5`.
- Comma-separated elements in one arm match if **any** of them matches, and each
  element may itself be a value or a range: `97..=122, 65..=90 -> "letter"`.
- Ranges are over integer ordinals (`int`, `long`, `byte`). Aether has no
  character literals, so a character class matches the byte's integer value,
  e.g. from `string.char_at`:

```aether,fragment
kind = match string.char_at(s, i) {
    97..=122, 65..=90 -> "letter"   // a-z, A-Z
    48..=57           -> "digit"    // 0-9
    _                 -> "other"
}
```

A ranged arm lowers to a plain comparison (`x >= lo && x <= hi`) in the branch
chain, with no runtime support and no allocation. Existing single-literal arms
are unaffected.

### List Pattern Matching

Arrays can be matched with list patterns. Requires a corresponding `_len` variable:

```aether,fragment
nums = [1, 2, 3];
nums_len = 3;

match (nums) {
    [] -> { print("empty list\n"); }
    [x] -> {
        print("single element: ");
        print(x);
        print("\n");
    }
    [a, b] -> {
        print("pair: ");
        print(a);
        print(", ");
        print(b);
        print("\n");
    }
    [h|t] -> {
        print("head: ");
        print(h);
        print(", tail has rest\n");
    }
}
```

### List Pattern Types

| Pattern | Matches | Bindings |
|---------|---------|----------|
| `[]` | Empty array | None |
| `[x]` | Single-element array | `x` = element |
| `[x, y]` | Two-element array | `x`, `y` = elements |
| `[x, y, z]` | Three-element array | `x`, `y`, `z` = elements |
| `[h\|t]` | Non-empty array | `h` = first, `t` = rest |

---

## Switch Statements

C-style switch for simple value dispatch:

```aether,fragment
switch (month) {
    case 1: name = "January";
    case 2: name = "February";
    case 3: name = "March";
    // ... more cases
    default: name = "Invalid";
}
```

Aether's `switch` has **no fall-through**: each case auto-breaks after its body.
Cases also accept comma-lists and ranges, the same as `match` arms:

```aether,fragment
switch code {
case 200, 201, 204: kind = "success";
case 300..<400:     kind = "redirect";   // ranged case (lowered to an if-chain)
case 400..<500:     kind = "client-error";
default:            kind = "other";
}
```

A comma-list case lowers to several C `case` labels sharing one body; a `switch`
containing any ranged case is lowered to an equivalent if-else chain (safe
because there is no fall-through to preserve).

### Switch vs Match

| Feature | `switch` | `match` |
|---------|----------|---------|
| Pattern types | Literals, ranges, comma-lists | Literals, ranges, comma-lists, lists, wildcards |
| Binding | No | Yes (captures variables) |
| Use case | Simple dispatch | Pattern destructuring |

---

## Defer Statement

The `defer` statement schedules code to run when leaving the current scope:

```aether,fragment
process_file() {
    handle = open_resource();
    defer close_resource(handle);  // Runs when function exits

    use_resource(handle);
    use_resource(handle);
    // close_resource(handle) called automatically here
}
```

### LIFO Order

Multiple defers execute in Last-In-First-Out order:

```aether,fragment
example() {
    defer print("First\n");   // Runs third
    defer print("Second\n");  // Runs second
    defer print("Third\n");   // Runs first
}
// Output: Third, Second, First
```

### `defer try` and `defer catch`, cleanup for one outcome only

A plain `defer` runs on **every** exit. Two qualified forms run on only one:

```aether,fragment
defer       cleanup()    // always — on every exit
defer try   commit()     // only when the function returns SUCCESSFULLY
defer catch rollback()   // only when the function returns an ERROR
```

"Error" means the function returned a **non-empty error slot**, Aether's
`(value, err)` convention, and `T!`, which is the same shape.

Together they give you the transactional shape in three lines: acquire, register
the rollback, register the commit, and then let any error path bail out without
the acquire leaking and without a half-built result being published.

```aether,fragment
acquire(path: string) -> (ptr, string) {
    p = malloc(SIZE)
    defer catch free(p)              // we bailed — release it
    cfg, err = parse(path)
    if err != "" { return null, err } // ...and `p` is freed on the way out
    init(p, cfg)
    return p, ""                     // succeeded — the caller owns `p` now
}
```

Without them, every early return needs its own `if err != "" { free(p); return }`,
and the one you forget is the leak.

Ordering is unchanged: **all** defers still run in LIFO order, and the
conditional ones interleave with the unconditional ones by registration order
they are not hoisted into separate groups. In

```aether,fragment
defer       log("A")
defer catch log("C")
defer try   log("T")
```

a successful return logs `T`, `A`; an error return logs `C`, `A`.

**Cost.** One predictable compare on the return path, and not even that where
the outcome is known at compile time. An `expr!` propagation is *always* an error
exit and a bare `return v` is *always* a success exit, so in a `T!` function both
are resolved statically and no guard is emitted at all. There is no runtime defer
stack; as with a plain `defer`, the bodies are emitted inline at each exit.

**They only mean something in a function that can fail.** In a function with no
error channel, a `defer catch` could never fire and a `defer try` is just a plain
`defer`, so the compiler warns rather than silently accepting code that does not
do what it says:

```
warning: `defer catch` in a function that cannot fail: it will never run.
Give the function an error result (`-> (T, string)` or `-> T!`), or use a plain `defer`.
```

### Use Cases

- Resource cleanup (files, connections)
- Unlocking mutexes
- Logging function exit
- Guaranteed cleanup regardless of return path
- **Rollback on failure / commit on success** (`defer catch` / `defer try`)

---

## Scope Directives: `hide` and `seal except`

Two scope-level directives let a block decline to see selected names from its enclosing lexical scopes. Both are compile-time only, no runtime overhead, no codegen change. Error code: `E0304`.

### `hide` blacklist specific names

```aether,fragment
{
    hide secret_token, db_handle
    // secret_token and db_handle from outer scopes are invisible here.
    // Reading, writing, or redeclaring them is a compile error.
}
```

### `seal except` whitelist

```aether,fragment
handler(req, res) {
    seal except req, res, inventory, response_write
    // Every outer name is invisible EXCEPT the four listed.
    // Local bindings created inside this block are still visible.
}
```

### Key semantics

A directive is scope-level: its position within the block doesn't matter, and it
blocks both reads and writes of the affected names. The denial propagates into
nested blocks, with no way to un-hide deeper in. It does not reach through call
boundaries, though: a visible function can still use hidden names via its own
lexical chain, since this is name-resolution denial, not an effect system. Local
bindings created inside the block stay visible, because directives only affect
lookups that walk out to parent scopes. Qualified names are covered too, so
`hide http` also blocks `http.get(url)`. The directives work inside actor receive
arms (handler bodies are block scopes), and a hidden name cannot be redeclared in
the same scope, though a nested child scope may.

For the full design rationale, edge cases, and worked examples, see [hide-and-seal.md](hide-and-seal.md).

---

## Memory Management

Aether uses **deterministic scope-exit cleanup** -- no garbage collector, no GC pauses. The primary mechanism is `defer`.

### `defer` for Cleanup (default)

Allocate, immediately defer the free, then use the resource. Cleanup runs at scope exit in LIFO order:

```aether
import std.list

main() {
    items = list.new()
    defer list.free(items)

    list.add(items, "hello")
    print(list.size(items))
    print("\n")
    // list.free(items) runs here (scope exit)
}
```

This works with any function, not just stdlib types.

### Returning Allocated Values

The caller receives ownership and is responsible for cleanup:

```aether
import std.list

build_list(n) : ptr {
    result = list.new()
    i = 0
    while i < n {
        list.add(result, i)
        i = i + 1
    }
    return result
}

main() {
    items = build_list(10)
    defer list.free(items)

    print(list.size(items))
    print("\n")
}
```

See [Memory Management Guide](memory-management.md) for the full reference.

### Multiple Return Values (Result Types)

Functions can return multiple values using comma-separated returns:

```aether
safe_divide(a: int, b: int) -> {
    if b == 0 {
        return 0, "division by zero"
    }
    return a / b, ""
}

main() {
    // Check error first, handle it, then continue, no else needed
    result, err = safe_divide(10, 3)
    if err != "" {
        println("Error: ${err}")
        exit(1)
    }
    println("Result: ${result}")

    // Discard unwanted values with _
    val, _ = safe_divide(42, 7)
    println(val)
}
```

The error convention: empty string `""` means no error, non-empty string is the error message. Use `!= ""` to check for errors.

Tuple destructuring creates typed local variables. The compiler generates C structs for each unique tuple type (`_tuple_int_string`, etc.).

#### Explicit tuple return type

The compiler infers the return tuple from `return a, b` statements in the body, but you can also declare it up front using the same parenthesised form accepted on `extern`:

```aether,fragment
safe_divide(a: int, b: int) -> (int, string) {
    if b == 0 { return 0, "division by zero" }
    return a / b, ""
}
```

Stating the return type at the signature is preferred when the function is part of a public API or when readers shouldn't have to scan the body to know the return shape. The two forms (`-> { ... }` with inference vs. `-> (T1, T2) { ... }` explicit) are interchangeable from the caller's perspective and produce the same C struct return.

Error propagation across function boundaries works correctly:

```aether,fragment
checked_op(x: int) -> {
    val, err = safe_divide(x, 2)
    if err != "" {
        return 0, err    // propagate the error
    }
    return val, ""
}
```

#### The `T!` result type and `or` / `!` sugar

`-> T!` is shorthand for the `(T, string)` result tuple above, it *names* the
convention and unlocks ergonomic sugar for the three things you do with a
fallible call. There is no hidden machinery: `T!` is the same tuple you can
always destructure by hand, so the sugar and the manual form interoperate
freely.

```aether,fragment
// `return v` auto-wraps to (v, ""); the error path is explicit.
safe_divide(a: int, b: int) -> int! {
    if b == 0 { return 0, "division by zero" }
    return a / b
}
```

**`expr or default` use the value, or a fallback on error:**

```aether,fragment
q = safe_divide(10, 0) or -1      // q == -1
```

**`expr or { ... }` run a block on error, with `err` bound to the message.**
The block's **last statement is its value**, so a handler can log, compute,
and still yield a fallback, or the block exits (`return`, `break`,
`continue`, `panic`) and never falls through:

```aether,fragment
q = safe_divide(x, y) or {
    println("math failed: ${err}")   // `err` is the error string
    return
}

r = safe_divide(x, y) or {
    println("using fallback: ${err}")
    -1                                // the block's value on the error path
}
```

A block that neither yields a value of the right type nor exits is a compile
error (`` `or { }` handler must end with a value … or exit ``), previously
that shape compiled and read an **uninitialized** result on the error path.

**`expr!` propagate the error.** Inside a function that itself returns `T!`,
`!` returns `(zero, err)` to the caller when the error slot is non-empty, so an
inner failure short-circuits without manual `if err != ""` plumbing:

```aether,fragment
checked_divide(x: int, d: int) -> int! {
    return safe_divide(x, d)!   // forwards safe_divide's error unchanged
}
```

Used outside a `T!` function, `expr!` is *unwrap-or-trap*: it panics on a
non-empty error slot (catchable with `try`/`catch`). Both `!` and `or` accept
any `(value, err)` tuple, not just `T!` returns.

### Function contracts: `requires` / `ensures`

Eiffel-style runtime-checked preconditions and postconditions. Clauses appear after the typed return arrow and before the body; each is a single boolean expression, panic on violation.

```aether,fragment
add(a: int, b: int) -> int
    requires a >= 0
    requires b >= 0
    ensures result >= a
    ensures result >= b
{
    return a + b
}
```

**Lowering**:


#### Panic categories

Every unrecoverable failure prints a stable category token, then a colon, then
the human detail and the source location. The token is the contract: CI and
downstream triage match on it, so it does not change with the prose after it.

| category | raised by |
|---|---|
| `precondition_violation` | a `requires` clause that evaluated false |
| `postcondition_violation` | an `ensures` clause that evaluated false |
| `forced_unwrap_none` | `x!` on an optional holding `none` |

A failure that forwards an existing error value (`expr!` on a fallible) prints
that error's own message rather than a category, because the message came from
the callee and is not one of these classes.

- `requires <expr>` lowers to `if (!(<expr>)) aether_panic("precondition_violation: <expr> in <fn>");` emitted at function entry, after parameters are declared and before any user code runs. Parameters are in scope.
- `ensures <expr>` lowers to a pre-return wrapper `{ <T> result = <return-expr>; if (!(<expr>)) aether_panic("postcondition violation: <expr> in <fn>"); return result; }` emitted before each `return` statement. The synthetic `result` local is the return value about to be returned and is scoped to the wrapper block, it shadows any outer `result` cleanly.

Multiple clauses in any order, freely interleaved. Each is checked independently, so the panic message names the specific failed predicate. The diagnostic plays nicely with `panic` stack traces for actionable error reporting.

**`--no-contracts` build flag**:

```sh
aetherc --no-contracts script.ae out.c    # zero per-call cost
```

Suppresses contract-check emission entirely. Equivalent to C's `-DNDEBUG` for `assert`. Intended for release builds where the contracts have been validated upstream.

**Compile-time folding** (design: [docs/contract-folding.md](contract-folding.md)):
predicates are evaluated at compile time whenever every operand is a
compile-time constant, literals, top-level `const` names, enum members,
arithmetic (`+ - * / %`, exact in int64), comparisons, `&& || !`. Three
outcomes:

- **Provably true** → the runtime check is **elided**; the emitted C carries a
  `/* precondition elided (always-true): <text> */` comment and is
  byte-for-byte identical to a function without the clause. This includes
  const-operand predicates (`requires cap > MIN_CAP` with a `const MIN_CAP`),
  not just literal ones.
- **Provably false at the definition** → **compile error**: no call could ever
  satisfy the clause. The realistic way to hit this is a refactor that stales
  a constant.
- **Provably false at a call site**, with the actual arguments substituted for
  the parameters → **compile error at that call**:

  ```aether
  divide(a: int, b: int where b != 0) -> int { return a / b }

  divide(10, 0)     // error: precondition violation at compile time:
                    //        b != 0 in divide — this call's constant
                    //        arguments can never satisfy it
  divide(10, n)     // n is runtime → runtime check, exactly as before
  ```

  This is trait-bound/concepts-like checking from the contract syntax you
  already wrote. Anything the evaluator cannot decide, a runtime variable, a
  call, member access, a partial set of constant arguments, keeps today's
  runtime check and produces no diagnostic. Platform-dead code cannot
  false-positive: `when` arms are pruned before the typechecker runs.

  Compile-time contract errors fire **even under `--no-contracts`**, that
  flag removes runtime *checks*, not free compile-time correctness findings.
  If a provably-violating call is intentionally unreachable, route the value
  through a runtime variable; only constant arguments participate in folding.

**Limitations / out-of-scope for v1**:

- Postconditions are checked only at explicit `return <expr>` statements with a single value. Multi-value (tuple) returns and fall-off-the-end of void functions are not yet wrapped, calling `aether_panic` from those paths is a follow-up.
- Short-circuit folding is asymmetric on purpose. `x || true` (unknown `x`)
  keeps the runtime check, because evaluating `x` may carry a side effect the
  user expects to fire, but `true || x` elides, since the runtime would
  short-circuit past `x` anyway; and `x && false` may be reported
  always-false, because a false verdict never skips any runtime evaluation
  (it is either a build error or an emitted runtime check).
- Call-site folding sees only the call itself: arguments must be constants
  right there. A constant flowing through an intermediate variable or a
  helper function is not tracked (that would be dataflow analysis).
- The `--emit=lib` `aether_describe()` metadata doesn't yet surface contracts to FFI consumers; that's the next layer.

See [examples/basics/contracts.ae](../examples/basics/contracts.ae) for runnable demos.

---

### String ownership for `-> string` functions

Strings are special-cased in the memory model: every reassignment to a string variable that previously held a heap-allocated value frees the old buffer through a compiler-emitted wrapper. **You do not write `defer string.free(s)` for in-Aether assignments**, the compiler tracks ownership transitions automatically per [docs/memory-management.md "String memory model"](memory-management.md#string-memory-model-heap-string-tracker).

A user-defined function declared `-> string` is treated as **heap-returning** iff every return statement in its body yields a heap-string-expression (recursive structural escape analysis):

```aether,fragment
my_concat(a: string, b: string) -> string {
    return string.concat(a, b)        // heap-string-expr return → my_concat is heap-returning
}

s = ""
i = 0
while i < 1000000 {
    s = my_concat(s, "x")              // O(1) memory, old s freed automatically
    i = i + 1
}
```

A function returning a string literal, or whose returns mix heap and literal sources, is **not** recognised. The wrapper won't try to free its result. This is the conservative stance taken to avoid free-of-literal aborts:

```aether,fragment
banner(name: string) -> string {
    if string.length(name) == 0 {
        return "guest"                 // literal, banner is NOT heap-returning
    }
    return string.concat("hi ", name)  // heap, but the function as a whole isn't heap-returning
}
```

If you want every return from your `-> string` function to be heap (so the wrapper reclaims it), box any literals: `return string.concat("", "guest")` makes the literal return a heap-string-expression and pushes the function into the heap-returning set.

See [examples/basics/string-ownership.ae](../examples/basics/string-ownership.ae) for a runnable demonstration.

---

## Optionals

An optional type `T?` holds either a value of type `T` or the empty sentinel `none`. It is the type for "maybe a value", distinct from the `(value, err)` result convention, which is for *fallible* operations. Use `(value, err)` when an operation can fail and you want to say why; use `T?` when a value is simply present or absent (a missing map key, an empty list's first element, a search that found nothing).

```aether,fragment
let maybe: int? = 69        // a present value, implicitly wrapped
let empty: int? = none      // the absent sentinel
```

`T?` works for any element type, value types (`int?`, `float?`, `bool?`) and reference types (`string?`, `*Node?`) alike, with one uniform representation, so there is no ambiguity between "the value is a null pointer" and "the key was absent".

### `none` and equality

`none` is a reserved literal (like `true`, `false`, and `null`), it cannot be used as a variable name. Compare against it with `==` / `!=`:

```aether,fragment
if empty == none {
    println("absent")
}
if maybe != none {
    println("present")
}
```

Two optionals compare equal when they have the same presence and (when present)
equal values: `a == b` for `int?` compares the ints, and for `string?` compares
the **contents** (via the same header-aware string comparison the language uses
elsewhere), not the underlying pointers.

An optional is a **single** presence layer, `T??` is rejected at parse time
(*"nested optional `T??` is not supported"*). If you find yourself wanting one,
you almost certainly want the distinct "fallible-and-absent" spelling instead,
which composes from a result and an optional rather than nesting two optionals.

### Force-unwrap `!`

Postfix `!` yields the wrapped value, or panics if the optional is `none`:

```aether,fragment
let n: int = maybe!         // 69
let boom: int = empty!      // panics: "forced unwrap of `none`"
```

Use it only where absence is a programmer error. For a safe default, prefer `??`. (Postfix `!` is shared with the `(value, err)` unwrap-or-trap: on an optional it unwraps the value, on a tuple it unwraps the first slot and traps on a non-empty error, the meaning follows the operand type.)

### Null-coalesce `??`

`opt ?? default` yields the wrapped value when present, otherwise `default`. It binds tighter than arithmetic, so `5 + (opt ?? 7)` groups as expected:

```aether,fragment
let a: int = maybe ?? 0     // 69
let b: int = empty ?? 0     // 0
let c: int = 5 + (empty ?? 7)   // 12
```

### Optional chaining `?.`

`opt?.field` reads a field through an optional struct and is *none-propagating*: it yields `fieldT?`, which is `none` when the receiver is `none`. Combine with `??` to default the result:

```aether,fragment
struct Vec2 { x: int  y: int }

let v: Vec2? = Vec2 { x: 7, y: 8 }
let xm: int? = v?.x          // some(7)
let x:  int  = v?.x ?? 0     // 7

let nv: Vec2? = none
let zm: int? = nv?.x         // none, short-circuited
```

Chain *assignment* is a no-op when the receiver is `none`:

```aether,fragment
v?.x  = 42                   // writes, v is present
nv?.x = 99                   // skipped, nv is none, no crash
```

### Matching on optionals

`match` destructures an optional with the `none` and `some(binding)` arms, as a statement or as an expression:

```aether,fragment
match maybe {
    none    -> println("missing")
    some(v) -> println("got ${v}")
}

let label: string = match maybe {
    none    -> "missing"
    some(v) -> "got ${v}"
}
```

### Narrowing

A none-check on an optional variable **narrows** it in the guarded branch: inside
`if x != none { ... }` (and the `else` of `if x == none { ... } else { ... }`),
`x` is its inner type `T` and is used directly, without the `!` force-unwrap.
Presence is proven by the guard, so the runtime none-check is elided, this is a
compile-time analysis with zero runtime cost:

```aether,fragment
find(id: int) -> User?

let u: User? = find(42)
if u != none {
    println(u.name)        // no `!`, `u` is a User here
    greet(u)              // passed by value as User
}
```

Narrowing is refused (soundly, the branch is unchanged) when the guarded branch
reassigns the variable, or uses it with an optional-only operator (`== none`,
`!= none`, `!`, `??`, or `?.`), since those need the optional form. A nested
guard on the same variable still narrows its own innermost block.

The narrowed value flows through expressions, field access, and function
arguments. Assigning the narrowed variable *directly* to a pre-existing variable
(`existing = x`) is one spot the inner type does not yet propagate (the outer
variable keeps its optional type); use an expression (`existing = x + 0`) or a
fresh `let` binding there.

### Functions

Optionals flow through parameters and return types. A bare `T` (or `none`) is implicitly wrapped at the call site and in `return`:

```aether
first_positive(n: int) -> int? {
    if n > 0 {
        return n            // wrapped into int?
    }
    return none
}

describe(x: int?) -> string {
    let label: string = match x {
        none    -> "nothing"
        some(v) -> "got ${v}"
    }
    return label
}

main() {
    println(describe(5))          // "got 5", 5 wrapped into int?
    println(describe(none))       // "nothing"
    let r: int = first_positive(3)!   // 3
}
```

### Codegen

A value optional lowers to `typedef struct { int has; T val; } ae_opt_<T>` (one per concrete element type); `none` is `{0}`, a wrapped value is `{ .has = 1, .val = … }`. `!`, `??`, and `?.` lower to single-evaluation GCC statement-expressions, so an operand with side effects is evaluated exactly once.

---

## Enums

An **enum** is a named set of integer constants, a distinct type over `int` with
named members. It lowers to a C `typedef enum` with zero runtime cost.

```aether,fragment
enum Direction { North, East, South, West }        // implicit 0, 1, 2, 3
enum Errno { Ok = 0, NotFound = 2, Perm = 13 }      // explicit values
```

A member with no `= value` is the previous member's value plus one (the first
defaults to `0`), matching C. Members are separated by commas and/or newlines.

Members are referenced by their **qualified name**, `EnumName.Member`:

```aether,fragment
d = Direction.East              // d has type Direction
if d == Direction.East { ... }  // compare (nominal: same enum only)
```

An enum is used like any other type, on parameters, returns, and locals, and is
matched with member arms. Inside a `match` on an enum the member may be written
qualified (`Direction.North`) or bare (`North`), since the scrutinee already
fixes the enum:

```aether,fragment
label(d: Direction) -> string {
    return match d {
        North -> "N"
        East  -> "E"
        South -> "S"
        West  -> "W"
    }
}
```

An enum `match` is **exhaustive-checked**: if it covers every member, no `_` arm
is needed (as above); if a member is left uncovered and there is no `_`, it is a
compile error naming the missing members (the same guarantee sum types give). A
`_` arm still catches the rest when you want a default.

Because an enum is integer-backed, its members interconvert with integer scalars
(`x: int = Errno.Perm` gives `13`; `code == Errno.NotFound` compares as ints),
but two **different** enums are never compatible with each other.

Where the expected type is already a known enum, a member may be written **bare**,
without the enum prefix. This applies to a function argument, a typed
initializer, an assignment, a return, and either side of an enum comparison:

```aether,fragment
paint(c: Color) -> string { ... }

paint(North)                 // function argument
let c: Direction = North     // typed initializer
c = South                    // assignment
if c == North { ... }        // comparison (either order)
heading() -> Direction { return West }   // return
```

A real binding always wins: if a variable named `North` is in scope, `North`
refers to it, not the member. A bare name that is not a member of the expected
enum stays an ordinary "undefined variable" error.

### Enum-indexed arrays

`[E]T` is a fixed array with exactly one slot per member of enum `E`, indexed by
an `E` value rather than a raw integer. It is sized at compile time to the enum's
member range and lowers to a plain C array (zero runtime cost):

```aether,fragment
enum Dir { N, E, S, W }
const LABELS: [Dir]string = ["north", "east", "south", "west"]  // one per member

LABELS[Dir.E]        // "east", indexed by the enum member
var t: [Dir]string = ["n", "e", "s", "w"]
t[Dir.N] = "NORTH"   // element assignment
```

A positional literal supplies one value per member in declaration order; the
count must match. The index must be a value of `E` (a raw `int` is rejected), so
the type keeps the table and its keys in lockstep. With explicit member values
the array spans `0 ..= max value`, so a sparse enum reserves the intervening
slots. Array-typed function parameters and empty `[]` initialisers share the
current fixed-size-array limitations and are a separate follow-up.

## Bit Sets

A **bit set** is a set of members of an enum, `bit_set[E]`. It is backed by a
single unsigned 64-bit word (one bit per member, at the member's enum value), so
every set operation is a bitwise operation with zero runtime cost.

```aether,fragment
enum Perm { Read, Write, Exec }

let a = bit_set[Perm]{ Perm.Read, Perm.Write }   // a two-member set
let b = bit_set[Perm]{ Write, Exec }             // bare member names also work
let empty = bit_set[Perm]{}                       // the empty set
```

Inside a `bit_set[E]{ ... }` literal each element is a member of `E`, written
qualified (`Perm.Read`) or bare (`Read`). The type is written the same way it is
constructed, so a set is used like any other type, on locals, parameters, return
types, and struct fields:

```aether,fragment
grant(base: bit_set[Perm]) -> bit_set[Perm] {
    return base + bit_set[Perm]{ Perm.Exec }
}

struct File { name: string, perms: bit_set[Perm] }
```

The operators:

| Operation      | Syntax            | Result       | Lowers to        |
|----------------|-------------------|--------------|------------------|
| membership     | `Perm.Read in a`  | `bool`       | `(a >> Read) & 1`|
| union          | `a + b`           | `bit_set[E]` | `a \| b`         |
| difference     | `a - b`           | `bit_set[E]` | `a & ~b`         |
| subset         | `a <= b`          | `bool`       | `(a & b) == a`   |
| superset       | `a >= b`          | `bool`       | `(a & b) == b`   |
| equality       | `a == b` / `!=`   | `bool`       | `a == b`         |
| cardinality    | `card(a)`         | `int`        | `popcount(a)`    |

```aether,fragment
let u = a + b                         // union
let d = a - b                         // difference (members of a not in b)
if Perm.Exec in u { ... }             // membership test
if a <= u { ... }                     // is a a subset of u?
let n = card(u)                       // how many members are set
```

A bit set is **nominal** and strictly typed: it never implicitly converts to or
from an integer, and two bit sets interoperate only when they are over the same
enum. Members must have values in `0..63` (the width of the backing word). `card`
is a reserved call form, like `sizeof`, and applies only to a bit set.

## Bitstructs

A **bitstruct** is a named bit layout over a single unsigned integer. It is the
tool for packed headers, hardware registers, and wire formats.

```aether,fragment
bitstruct DnsFlags : uint16_t {
    qr:     bool 15          // a single bit
    opcode: int  11..=14     // an inclusive range: bits 11,12,13,14
    aa:     bool 10
    rcode:  int  0..<4       // an exclusive range: bits 0,1,2,3
}

f = 0 as DnsFlags
f.opcode = 2
f.qr = true
w = f as uint16_t            // 0x9000 | ...
```

**A bitstruct never lowers to a C bitfield.** It lowers to shift-and-mask
arithmetic on the backing integer. This is the whole point of the feature: C's
`:n` bitfields have implementation-defined signedness, allocation order, and
straddling behaviour, which makes them unusable for anything that has to match a
byte-exact layout. In particular gcc gives `int x : 3` a **signed**
representation, so a stored `0b111` reads back as `-1`. A bitstruct field cannot
do that, the backing word is unsigned and the mask is applied after the shift,
so there is nothing to sign-extend from.

The rules, each of which exists to keep the layout exact:

- **The backing type is mandatory** and must be one of `uint8_t`, `uint16_t`,
  `uint32_t`, `uint64_t`. Naming the storage explicitly is what fixes its width
  and its signedness. Omitting it is a compile error, not a default.
- **Bit positions are explicit.** A bare index (`0`) is a one-bit field. A range
  may be written inclusively (`1..=3`) or exclusively (`1..<4`), both denote the
  same three bits. Aether does not pick one for you (C3, which this borrows from,
  hardcodes inclusive ranges and leaves you to remember); the source says which
  it means, using the same `..=` / `..<` spellings as match-range labels.
- **Overlapping fields are an error** unless the bitstruct is annotated
  `@overlap`, which permits union-like views of the same bits.
- **A range that runs off the end of the backing integer is an error.**
- **Fields are `bool`, an integer type, or an enum.** A `bool` field is exactly
  one bit.
- **Writing a field never disturbs its neighbours.** An over-wide value truncates
  to its own field rather than bleeding into the next one.
- **A bitstruct is nominal.** It never implicitly converts to or from its backing
  integer, and two bitstructs over the same backing type are still different
  types. Crossing the boundary is an explicit `as` in either direction, a
  bitstruct is a packed layout, not a number you do arithmetic on.

### Bit layout and byte order are separate concerns

A bitstruct says **which bits**. It deliberately says nothing about **which byte
order**, there is no `@bigendian` annotation, and no hidden byte-swapping on
field access. Byte order is a property of *serialisation*, not of the layout, and
Aether already has endian-explicit accessors for it in [`std.mem`](../std/mem/):

```aether,fragment
import std.mem

// Read a big-endian word off the wire, then interpret its bits.
w    = mem.get_u16_be(buf, 0)
hdr  = w as DnsFlags
kind = hdr.opcode

// ...and back out again.
mem.set_u16_be(buf, 0, hdr as uint16_t)
```

Keeping the two apart means there is exactly one place where a byte swap can
happen, and it is visible in the source. Folding endianness into the type would
make the swap implicit and raise the question "does it happen on every field
read, or only at the byte boundary?", the sort of ambiguity that makes C
bitfields untrustworthy in the first place.

## Sum / Variant Types

A **sum type** is a value that is exactly one of N named struct variants:

```aether,fragment
struct Circle { r: float }
struct Rect   { w: float  h: float }
struct Empty  {}
type Shape = Circle | Rect | Empty
```

Each variant is an existing struct. A sum needs at least two variants (a
single-name `type T = A` alias is not a supported form).

### Constructing, implicit wrap

A variant struct value flows into a `Shape` slot directly; there is no
`some(...)`-style constructor. The wrap happens at `let` bindings, function
parameters, return values, and call arguments:

```aether,fragment
let s: Shape = Circle { r: 2.0 }      // Circle wraps into Shape
draw(Rect { w: 3.0, h: 4.0 })         // wraps at the argument
make() -> Shape { return Empty {} }   // wraps at the return
```

### Matching, narrowing + exhaustiveness

`match` over a sum dispatches on the variant and **narrows** the scrutinee to
that variant struct inside the arm, so `s.field` reads the right member:

```aether,fragment
area(s: Shape) -> float {
    let a: float = match s {
        Circle -> 3.14159 * s.r * s.r   // here `s` is a Circle
        Rect   -> s.w * s.h             // here `s` is a Rect
        Empty  -> 0.0
    }
    return a
}
```

The match must be **exhaustive**: every variant must be handled, or a `_`
wildcard arm must catch the rest. Omitting a variant is a compile error, the
payoff that tells you, when you add a variant, exactly which matches to update.
An arm naming something that isn't a variant is also rejected. Narrowing applies
when the scrutinee is a plain variable (`match s { … }`).

### Recursive sums

Self-referential shapes (trees, ASTs, JSON values) use explicit pointer fields:

```aether,fragment
struct Leaf { v: int }
struct Pair { left: *Tree  right: *Tree }
type Tree = Leaf | Pair
```

### Codegen

A sum lowers to a tag enum plus a tagged union, no allocation, no vtable:

```c
typedef enum { Shape__Circle, Shape__Rect, Shape__Empty } Shape_tag;
typedef struct Shape { Shape_tag tag; union { Circle Circle_; Rect Rect_; Empty Empty_; } data; } Shape;
```

`match` becomes a `switch` on `.tag` with the matched value narrowed to the
active union member in each `case`. v1 is monomorphic; type parameters are a
follow-up.

---

## Structs

Structs group related data:

```aether
struct Point {
    x,
    y
}

struct Person {
    name,
    age
}

main() {
    p = Point { x: 10, y: 20 };
    print(p.x);  // 10
    print(p.y);  // 20
}
```

### Explicit Field Types

```aether,fragment
struct Point {
    int x,
    int y
}

struct Config {
    string name,
    int timeout,
    float threshold
}
```

### Field injection with `using` (composition, no vtables)

A field declared `using embed: Sub` embeds a sub-struct and **promotes its
fields** into the outer struct's namespace. This is composition-over-inheritance
without method sets or vtables, a pure compile-time member-access rewrite:

```aether,fragment
struct Entity { x: int, y: int }

struct Frog {
    using entity: Entity    // promotes x, y
    hops: int
}

f = Frog { entity: Entity { x: 10, y: 20 }, hops: 3 }
f.x            // == f.entity.x  (reads through the embed)
f.x = 99       // writes through the embed too
f.entity.x     // the explicit path still works
```

`f.x`, when `x` is not a direct field of `Frog`, resolves to `f.entity.x` at
compile time (both reads and writes). A name that no direct or `using` field
provides is still a "no field" error. Zero runtime cost: the outer struct just
contains the embedded struct as an ordinary field.

Only the **field form** exists. Odin's `using` *statement* (dumping a struct's
fields into local scope inside a function body) is deliberately not adopted, it
is a readability footgun.

### Function-pointer struct fields, a vtable of callbacks

A struct field can be a function pointer (`fn(T1, T2) -> R`), the `dictType`-style vtable. The field emits as the C function-pointer member `R (*name)(T1, T2)`, and a call through it is a real indirect call:

```aether,fragment
struct Ops {
    hash:    fn(int) -> int
    combine: fn(int, int) -> int
}

main() {
    o = Ops { hash: my_hash as fn(int) -> int, combine: my_add as fn(int, int) -> int }
    o.hash(5)                       // value struct  → (o.hash)(5)
}

dispatch(p: *Ops, k: int) -> int {
    return p.hash(k)                // pointer struct → (p->hash)(k)
}
```

Assign each field an Aether function's address via `as fn(...)` (or a C function pointer from an extern). Dispatch works for a value struct (`o.field(args)`) and a pointer-to-struct (`p.field(args)` lowers to `p->field(args)`); the receiver must be a single local, and the field's signature is taken from the struct definition. This is the field form of the same typed-fn-pointer machinery as `as fn(...)` locals.

### Pointer-to-struct type, `*StructName` and `expr as *StructName`

For systems-programming code that overlays a struct header on a raw `ptr` (e.g. linked-list nodes in C-allocated memory, on-disk file headers read into a buffer), Aether has a first-class pointer-to-struct type spelled `*StructName` and a postfix `as` cast operator that produces a value of that type:

```aether
extern malloc(size: int) -> ptr
extern free(p: ptr)

struct ListHead {
    next: ptr
    prev: ptr
    flags: int
}

// `*ListHead` is usable in any type position, params, returns,
// struct fields, locals.
init_head(h: *ListHead) {
    h.next  = 0
    h.prev  = 0
    h.flags = 1
}

main() {
    raw  = malloc(64)
    head = raw as *ListHead    // type of head is *ListHead
    init_head(head)
    free(raw)
}
```

The cast is a view, not an allocation, the operand pointer's lifetime is the caller's problem (the same contract as raw `extern` interaction). Reach for this only when the storage is C-allocated and Aether wants to manipulate fields. For Aether-owned data, use the normal struct-literal form (`Point { x: 1, y: 2 }`) so refcounting and lifetime tracking apply.

**`as` accepts a primitive value cast, a struct overlay (`*StructName`), a function-pointer cast (`fn(...) -> R`), or a typed-array view (`T[]`).** A value cast like `n as int` (numeric to numeric, or between a distinct type and its base) compiles and runs. Non-numeric casts such as `buf as string` or `raw as ptr` parse but are rejected at type-check with `E0200`. For converting between primitive types, see the [Casting between types](#casting-between-types) table above, most conversions are either implicit (Aether's type system inserts the necessary cast in the generated C) or use a named helper (`string.from_int`, `string.from_long`, …).

The `as` keyword is the same token used for `import x as y` aliasing; the two parses don't collide because import-aliasing is recognised only inside `import` statements. Full semantics (operand type rules, error cases, the shared-token interaction) are in [c-interop.md § Struct overlay on raw pointers](c-interop.md#struct-overlay-on-raw-pointers-structname-and-expr-as-structname).

#### Constructors returning `*StructName`

The constructor shape, allocate raw storage, overlay the struct, initialise the fields, return the typed view, is the natural way to retire C "container holder" shims to Aether:

```aether,fragment
struct Txn {
    revision: int
    paths: ptr
    state: int
}

mk_txn(revision: int) -> *Txn {
    raw = malloc(64)
    t = raw as *Txn
    t.revision = revision
    t.paths    = 0
    t.state    = 0
    return t
}
```

The pointer-to-struct type is accepted in every type position, including the return-type position used here. Earlier compiler versions tripped on `-> *Foo` in this slot (the parser's typed-return-vs-arrow-body disambiguator missed `TOKEN_MULTIPLY`); current builds parse it cleanly.

#### Self-referential structs

Pointer-to-struct fields may point back at the enclosing struct, which is how recursive shapes (linked-list cells, error chains, n-ary tree nodes, dependency graphs) are spelled:

```aether,fragment
struct ErrChain {
    code:  int
    msg:   string
    cause: *ErrChain      // self-pointer, chain cell or null
    file:  string
    line:  int
}

mk_err(code: int, msg: string, cause: *ErrChain, file: string, line: int) -> *ErrChain {
    raw = malloc(64)
    e = raw as *ErrChain
    e.code  = code
    e.msg   = msg
    e.cause = cause
    e.file  = file
    e.line  = line
    return e
}

walk(e: *ErrChain) {
    if e == 0 { return }
    println("[${e.code}] ${e.msg} (${e.file}:${e.line})")
    walk(e.cause)
}
```

The recursion above reads naturally and is fine for the short chains errors usually form. But Aether does **not** turn tail calls into loops, so each cell costs one C stack frame: walking (or freeing) a `*Struct` chain that can grow without bound, a long linked list, a deep tree assembled from untrusted input, an error chain accumulated in a hot retry loop, risks a stack overflow. For unbounded shapes, walk the spine iteratively instead, which is O(1) stack at any depth, exactly as the refcounted `*StringSeq` does internally ([sequences.md](sequences.md) documents "O(n) time, O(1) stack" for 10k+ cells):

```aether,fragment
// Iterative traversal: O(1) stack regardless of depth.
cur = e
while cur != 0 {
    println("[${cur.code}] ${cur.msg} (${cur.file}:${cur.line})")
    cur = cur.cause
}
```

Lifetime is the operand's, `mk_err` owns the raw allocation it returned, and the caller is responsible for freeing the chain. Free it with the same iterative spine walk, capturing each cell's successor *before* freeing it so the walk never dereferences freed memory:

```aether,fragment
// Iterative free: capture next before freeing the current cell.
cur = e
while cur != 0 {
    next = cur.cause
    free(cur as ptr)
    cur = next
}
```

There is no generic library helper for this: each user shape names its link field differently (`cause`, `next`, `tail`, ...), and the loop is three lines. Prefer it over recursion for any chain whose length you do not control. (For Aether-managed lifetimes on a similar shape with refcount-aware structural sharing, see `*StringSeq` in [sequences.md](sequences.md).)

---

## Messages

Messages define structured data for actor communication:

```aether,fragment
message Increment {
    amount: int
}

message Greet {
    name: string
}

message SetPosition {
    x: int,
    y: int
}

message Reset {}  // Empty message
```

---

## Actors

Actors are the core concurrency primitive with encapsulated state and message handling.

### Actor Definition

```aether,fragment
actor Counter {
    state count = 0;

    receive {
        Increment(amount) -> {
            count = count + amount;
        }
        GetCount() -> {
            print(count);
            print("\n");
        }
        _ -> {
            print("Unknown message\n");
        }
    }
}
```

### Receive Timeouts

The `after` clause fires a handler if no message arrives within N milliseconds:

```aether,fragment
actor Monitor {
    state alive = 1

    receive {
        Heartbeat -> { alive = 1 }
    } after 5000 -> {
        println("No heartbeat for 5 seconds")
        alive = 0
    }
}
```

The timeout is one-shot: it is cancelled when any message is received. The countdown starts when the actor's mailbox becomes empty.

### State Variables

State persists across messages:

```aether,fragment
actor BankAccount {
    state balance = 0;
    state transactions = 0;
    state int[100] history;
}
```

### Spawning Actors

```aether,fragment
counter = spawn(Counter());
calculator = spawn(Calculator());
```

### Sending Messages (Fire-and-Forget)

```aether,fragment
counter ! Increment { amount: 10 };
counter ! Reset {};
```

### Ask Pattern (Request-Reply)

The `?` operator sends a message and blocks until the actor replies. The compiler
infers the reply type from the actor's receive handler, so the result is typed
without an annotation. Multiple concurrent asks to the same actor are supported,
each message carries its own reply slot.

```aether,fragment
// Synchronous request-reply, result is an int (from Result.value)
result = calculator ? Add { a: 5, b: 3 };
```

A handler replies in one of two ways, and the ask takes its type from whichever
it used:

```aether,fragment
message GetName {}
message GetSize {}
message SizeReply { bytes: int }

actor Store {
    state name = "primary"
    receive {
        GetName() -> { reply name }                    // bare expression
        GetSize() -> { reply SizeReply { bytes: 4096 } }  // reply message
    }
}

label = store ? GetName {}   // string, from the replied expression
size  = store ? GetSize {}   // int, from the reply message's first field
```

A reply message hands back its **first declared field**, so put the value there.
A string reply is deep-copied before it crosses back, so the asker's copy
outlives the handler's own scope, and the asker owns it: it is reclaimed at
scope exit like any other owned string, and freeing it by hand is not
required.

If the handler does not call `reply` within the timeout (default 5 seconds), `?`
returns 0.

### Reply Statement

The `reply` statement sends a response back to the waiting `?` caller. Omitting
`reply` in a handler that was invoked via `?` causes the caller to time out.

Actors respond using the `reply` statement:

```aether,fragment
actor Calculator {
    receive {
        Add(a, b) -> {
            result = a + b;
            reply Result { value: result };
        }
    }
}
```

### Wildcard Handler

The `_` pattern catches unmatched messages:

```aether,fragment
receive {
    Known() -> { /* handle */ }
    _ -> { print("Unknown message\n"); }
}
```

---

## Operators

### Arithmetic Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `+` | Addition | `a + b` |
| `-` | Subtraction | `a - b` |
| `*` | Multiplication | `a * b` |
| `/` | Division | `a / b` |
| `%` | Modulo | `a % b` |

### Comparison Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `==` | Equal | `a == b` |
| `!=` | Not equal | `a != b` |
| `<` | Less than | `a < b` |
| `>` | Greater than | `a > b` |
| `<=` | Less or equal | `a <= b` |
| `>=` | Greater or equal | `a >= b` |

> **String comparison:** When both operands are strings, `==` and `!=` compare by content (using `strcmp` in the generated C), not by pointer identity. Two strings with the same content are always equal regardless of how they were allocated.

### Bitwise Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `&` | Bitwise AND | `flags & mask` |
| `\|` | Bitwise OR | `flags \| bit` |
| `^` | Bitwise XOR | `a ^ b` |
| `~` | Bitwise NOT | `~mask` |
| `<<` | Left shift | `1 << 4` |
| `>>` | Right shift | `n >> 2` |

Bitwise operators work on `int` and `long` values and map directly to C operators (zero runtime cost).

```aether,fragment
flags = 255
mask = flags & 15       // 15
set = flags | 256       // 511
flipped = flags ^ 255   // 0
shifted = 1 << 4        // 16
```

### Logical Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `&&` | Logical AND | `a && b` |
| `\|\|` | Logical OR | `a \|\| b` |
| `!` | Logical NOT | `!a` |

### Assignment Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `=` | Assignment | `x = 5` |
| `+=` | Add and assign | `x += 5` |
| `-=` | Subtract and assign | `x -= 5` |
| `*=` | Multiply and assign | `x *= 5` |
| `/=` | Divide and assign | `x /= 5` |
| `%=` | Modulo and assign | `x %= 5` |
| `&=` | Bitwise AND assign | `x &= mask` |
| `\|=` | Bitwise OR assign | `x \|= bit` |
| `^=` | Bitwise XOR assign | `x ^= mask` |
| `<<=` | Left shift assign | `x <<= 4` |
| `>>=` | Right shift assign | `x >>= 2` |

### Postfix Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `++` | Post-increment | `i++` |
| `--` | Post-decrement | `i--` |

### Actor Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `!` | Send message (async) | `actor ! Msg {}` |
| `?` | Ask (request-reply) | `actor ? Query {}` |

### Member Access

| Operator | Description | Example |
|----------|-------------|---------|
| `.` | Field access | `point.x` |
| `[]` | Array index | `arr[0]` |

### Operator Precedence (High to Low)

1. `()` `[]` `.` - Grouping, indexing, member access
2. `!` `-` `~` (unary) `++` `--` - Unary operators
3. `*` `/` `%` - Multiplicative
4. `+` `-` - Additive
5. `<<` `>>` - Bitwise shift
6. `<` `>` `<=` `>=` - Relational
7. `==` `!=` - Equality
8. `&` - Bitwise AND
9. `^` - Bitwise XOR
10. `|` - Bitwise OR
11. `&&` - Logical AND
12. `||` - Logical OR
13. `=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=` - Assignment
14. `!` `?` - Actor send/ask

---

## Modules and Imports

### Standard Library Imports

```aether,fragment
import std.file;         // File operations
import std.string;       // String utilities
import std.list;         // ArrayList
import std.http;         // HTTP client & server
import std.json;         // JSON parsing

// Use with namespace syntax
result = string.new("hello");
if (file.exists("config.txt") == 1) { }
```

### Selective Imports

Import only specific symbols from a module:

```aether
import std.math (sqrt, pow)

main() {
    x = math.sqrt(16.0)    // works
    y = math.pow(2.0, 3.0) // works
    // math.sin(1.0)       // error: not imported
}
```

If `sqrt` internally calls a sibling helper that *isn't* in the import list, the helper is still pulled into the merged build so the imported function can resolve its calls. Only the names you actually listed are visible to your code; the transitive pull-in is bookkeeping the compiler does on your behalf.

### Module Public API, `exports (…)`

Each module declares its public surface once at the top of the file via
an `exports (…)` list. Names in the list are callable from
outside the module via either qualified (`mod.name(…)`) or short-alias
(`import mod (*)`) forms. Names **not** in the list are private, still
callable from inside the module's own functions, but rejected at
qualified-call sites from outside.

```aether,fragment
// At the top of greeter/module.ae:
exports (say_hello, greet_world, GREETING)

const GREETING = "hello"

say_hello() {
    return GREETING                  // public, listed
}

greet_world() {
    return _format(GREETING, "world")  // public, listed; calls private helper
}

// Not listed → private. Leading underscore is a naming convention only;
// the exports list is the contract that actually enforces visibility.
_format(prefix: string, target: string) {
    return "${prefix} ${target}"
}
```

The form is *additive* in v1: a module without an `exports (…)` list
keeps the legacy default-public behavior so existing code continues to
work unchanged. v2 will flip the default to private once every module
in `std/` and `contrib/` has been migrated.

The legacy per-function `export <fn>` form is still accepted but emits
a one-shot deprecation warning per module. Migrate by collecting every
`export`-tagged name into a single `exports (…)` line at the top, then
removing the `export` keywords from each declaration. Mixing both forms
in one module is a hard error.

### Glob Import, `import mod (*)` (a.k.a. unqualified import)

Expose **every public name** in a module as an unqualified short alias,
without enumerating each symbol individually. Names with a leading
underscore (`_helper`, `_internal`) stay private and are not aliased.
This is Aether's spelling for what other languages call "unqualified
import" (Rust's `use mod::*;`, Java's `import static mod.*`, Python's
`from mod import *`).

```aether
import std.math (*)

main() {
    x = sqrt(16.0)         // works, short alias registered
    y = pow(2.0, 3.0)      // works
    z = math.sin(1.0)      // qualified form still works alongside the glob
}
```

It also applies to any module with a wide, short-named surface, e.g.
`std.bits`, where unqualified `popcount32(...)` / `rotl32(...)` /
`clz32(...)` reads better than the namespace-prefixed form:

```aether
import std.bits (*)

main() {
    println(popcount32(0xFF))    // 8, one-bits set
    println(rotl32(1, 4))        // 16, rotate left
    println(clz32(1))            // 31, leading zeros
    println(bits.lsr32(256, 4))  // 16, qualified form still works
}
```

The glob form composes across module boundaries: a library module that
does `import std.fs (*)` and calls a glob-brought `clean(...)` resolves
correctly whether it is the compilation entry point or is imported by
another module. (The bare glob-brought names are rewritten to their
canonical prefixed form when the module is merged into a consumer, the
same way selective and qualified imports are.)

Use the glob form when you'd otherwise list 20+ symbols just to use
the module without the namespace prefix. Bare `import std.math` (no
parens) loads the module but does **not** register short aliases,
you have to write `math.sqrt(...)` for everything. The selective form
`import std.math (sqrt, pow)` aliases only the named symbols and
keeps everything else qualified, the same shape as `import static`
in Java, useful when you want exactly two or three symbols bare and
the rest namespaced.

**The qualified `mod.fn(...)` surface is always available** whenever a
module is imported in *any* form, bare, selective, or glob, the way
Java's fully-qualified name is always legal regardless of imports. A
selective import is purely **additive**: `import std.math (sqrt)` adds the
bare-name binding `sqrt(...)` *on top of* the always-available qualified
surface, so `math.pow(2.0, 3.0)` still resolves even though `pow` was not
in the selection list. The import form decides only which **bare** names
exist; it never restricts the qualified form.

```aether
import std.math (sqrt)

main() {
    a = sqrt(16.0)            // bare-name binding the selective import adds
    b = math.pow(2.0, 3.0)    // qualified, always available, even un-selected
}
```

### Import with Alias

The `import mod as alias` syntax parses and records the alias, but calls through
the alias prefix (`str.new(...)`) do not currently resolve, the aliased name is
not wired into namespace lookup, so the call fails typechecking. Until that lands,
use the module's own namespace directly:

```aether,fragment
// Aliased-call form parses but does not yet resolve:
// import std.string as str
// s = str.new("hello")            // typecheck error: undefined function

// Working form: use the module name directly
import std.string
s = string.new("hello")
len = string.length(s)
string.release(s)
```

### Local Module Imports

```aether,fragment
import utils;           // Loads lib/utils/module.ae
import helpers;         // Loads lib/helpers/module.ae

result = utils.double_value(21);
```

### Available Standard Library Modules

| Module | Namespace | Description |
|--------|-----------|-------------|
| `std.file` | `file` | File operations (`file.open()`, `file.exists()`) |
| `std.dir` | `dir` | Directory operations (`dir.list()`, `dir.create()`) |
| `std.path` | `path` | Path utilities (`path.join()`, `path.basename()`) |
| `std.string` | `string` | String manipulation (`string.new()`, `string.length()`) |
| `std.list` | `list` | Dynamic array (`list.new()`, `list.add()`) |
| `std.map` | `map` | Hash map (`map.new()`, `map.put()`) |
| `std.set` | `set` | Unique-string set (`set.new()`, `set.add()`) |
| `std.pqueue` | `pqueue` | Priority queue (`pqueue.new()`, `pqueue.push()`) |
| `std.json` | `json` | JSON encoding/decoding (`json.parse()`, `json.free()`) |
| `std.http` | `http` | HTTP client & server (`http.get()`, `http.server_create()`) |
| `std.tcp` | `tcp` | TCP sockets (`tcp.connect()`, `tcp.write()`) |
| `std.math` | `math` | Math functions (`math.sqrt()`, `math.sin()`) |
| `std.log` | `log` | Logging utilities (`log.init()`, `log.write()`) |
| `std.io` | `io` | Input/output (`io.print()`, `io.getenv()`) |

---

## Extern Functions

Declare external C functions:

```aether
extern puts(s: string) -> int
extern malloc(size: int) -> ptr
extern free(p: ptr) -> void

main() {
    puts("Direct C call!")
}
```

Externs are useful for:
- Calling C standard library
- Custom C extensions
- Platform-specific APIs

### Typed and qualified C pointers

Plain `ptr` lowers to C `void *`. C headers use richer pointer spellings, and when an Aether `extern` (or an Aether-owned public function) must match a header **exactly**, so the generated C compiles with that header force-included, and the C compiler still cross-checks the signature against every caller, use these forms. Each affects only the *emitted C spelling*; the Aether-side `kind` (and so all typechecking) is unchanged.

| Aether spelling | Emitted C | For |
|---|---|---|
| `const ptr` | `const void*` | `memcmp(const void *, const void *, size_t)` |
| `cstring` | `char*` | a mutable C-string parameter |
| `cstring_const` | `const char*` | `strlen(const char *)` (plain `string` also emits `const char*`) |
| `*T` | `T*` | a pointer to a header-defined type `T` |
| `const *T` | `const T*` | a const pointer to `T` |
| `const string` | `const char*` | qualified string |

```aether,fragment
extern type dictEntry                              // opaque header-defined C type
extern dictGenHashFunction(key: const ptr, len: size_t) -> uint64_t
extern strlen(s: cstring_const) -> size_t
extern dictGetKey(de: *dictEntry) -> ptr

// An Aether-OWNED public symbol whose prototype must match its header:
pqsort(a: ptr, n: size_t, es: size_t, cmp: const ptr, lr: size_t, rr: size_t) { ... }
```

Passing a plain `ptr` where the C conversion is safe stays allowed at call sites; only the *emitted prototype* carries the exact spelling. C ABI scalar aliases (`size_t`, `uint64_t`, …) emit their exact C name the same way. `const`-qualification survives into the generated C so the C compiler diagnoses writes; Aether-side write rejection is not (yet) enforced.

### `@extern("c_name")` bind to a renamed C symbol

When the Aether-side name should differ from the C symbol (for example, to expose a clean module surface without trailing `_raw` suffixes), prefix the declaration with `@extern("c_symbol")`:

```aether,fragment
@extern("EVP_MD_CTX_new") md_ctx_new() -> ptr
@extern("strerror") describe_errno(errno: int) -> string
```

The Aether-side name is what callers write; the annotated C symbol is what the linker sees. No wrapper function is emitted. See [`docs/c-interop.md`](c-interop.md#renaming-a-c-symbol-externc_name) for the full FFI reference.

### `extern const NAME: type @c_import` import an object-like C macro

Object-like C macros (`EAGAIN`, `O_NONBLOCK`, `LLONG_MAX`, a generated `REDIS_GIT_SHA1`, …) are invisible to Aether, there is no symbol to link against. `extern const … @c_import` makes one usable by **name**:

```aether,fragment
extern const EAGAIN: int @c_import
extern const O_NONBLOCK: int @c_import
extern const REDIS_GIT_SHA1: ptr @c_import   // string macro -> const char *

if syncio_errno() == EAGAIN { ... }
```

- The declaration teaches the typechecker a name and an Aether type; **the type is trusted as declared**, the same model as `extern` functions.
- Generated C emits the macro name **verbatim** at every use site and emits **nothing** for the declaration itself, no value, no `#define`, no forward declaration. The macro's value is never needed at Aether compile time, so per-platform values come out right by construction: the including translation unit's headers are the sole source of truth. Ensure the owning header is in scope (e.g. `cflags = "-include errno.h"` in `aether.toml`, or a header your build already force-includes).
- Usable in expression and comparison contexts. `@c_import` is required, it is the marker that selects the emit-verbatim semantics.
- **Object-like macros only.** Function-like macros (`CPU_SET(i, &set)`) are out of scope; wrap those in a small `extern` C function.

### `extern ... @c_import` let a C header own the prototype

By default Aether emits its own forward declaration for every `extern`. That declaration is ABI-compatible with the header's but not always spelled the same: `int` where the header says `uint8_t`, `int` where it says `size_t`, `void*` where it says a typed pointer. Two differently spelled declarations of one function in the same translation unit are a hard `conflicting types` error, and under LTO the surviving cases show up as type-mismatch warnings that are hard to tell apart from real porting mistakes.

`@c_import` after the signature says the header owns the prototype, so Aether emits **none**:

```aether,fragment
extern type blob

extern api_scale(v: int) -> int          @c_import   // header: uint8_t api_scale(uint8_t)
extern api_span(a: int, b: int) -> int   @c_import   // header: size_t api_span(size_t, size_t)
extern api_blob() -> *blob               @c_import
extern api_inline_twice(v: int) -> int   @c_import   // static inline in the header
```

- The Aether-side types are still declared, and are still what the typechecker uses, **the type is trusted as declared**, the same model as every other `extern`. They just never reach the generated C.
- The header's spelling ends up being the only declaration in the translation unit, so the two cannot disagree. The C compiler still checks every call site against the header, so a genuinely wrong Aether signature is still caught, at the call site rather than as a prototype clash.
- Ensure the owning header is in scope, e.g. `cflags = "-include api.h"` in `aether.toml`. Same requirement as `extern const @c_import` and `extern struct @c_import`.
- This is the only correct shape for a **`static inline`** header helper: it has no linkable symbol, so a non-static prototype referring to it is meaningless. With `@c_import` the call inlines directly and needs no hand-written C shim.
- Stacks with the other extern attributes: `-> string @heap @c_import` and variadic `extern printf(fmt: string, ...) -> int @c_import` are both legal.

### `va_list` forward a variadic tail to C

A function declared with a trailing `...` can open its variadic tail and hand it to the `v*` half of a printf-style C pair:

```aether,fragment
extern vsnprintf(buf: ptr, n: int, fmt: ptr, ap: va_list) -> int

format_into(buf: ptr, n: int, fmt: ptr, ...) -> int {
    let ap = va_start()
    let written = vsnprintf(buf, n, fmt, ap)
    va_end(ap)
    return written
}
```

- `va_start()` opens the tail (valid only inside a `...` function), `va_arg(ap, T)` reads the next argument as `T`, `va_end(ap)` closes it. The type given to `va_arg` is trusted, exactly as in C.
- **The receiving parameter must be spelled `va_list`, not `ptr`.** `va_start()` yields a cookie that points at the `va_list` so it can live in an ordinary `ptr` local, while the callee needs the list itself. Declaring the parameter `ptr` compiles clean and then formats garbage. `va_list` typechecks as an opaque `ptr`; only the emitted C spelling differs.

See [`docs/c-interop.md`](c-interop.md#forwarding-a-variadic-tail-to-c-va_list) for the full treatment, including `-Wformat` checking of literal format strings.

### `@c_callback` export an Aether function as a C callback

The inverse of `@extern`. Marks an Aether function as having a stable, externally-visible C symbol so it can be passed across the linkage boundary as a function pointer to C externs that take callbacks (HTTP route handlers, signal handlers, `qsort` comparators, libcurl write callbacks, sqlite hooks):

```aether,fragment
extern http_server_add_route(server: ptr, method: string, path: string, handler: ptr, user_data: ptr)

@c_callback
my_handler(req: ptr, res: ptr, ud: ptr) {
    // …
}

main() {
    http_server_add_route(server, "GET", "/hello", my_handler, null)
}
```

By default the C symbol matches the Aether-side name (or its namespace-prefixed form when the function lives in an imported module). For a specific C symbol, use the parenthesised form: `@c_callback("aether_signal_handler") on_sigint(sig: int) { … }`. See [`docs/c-interop.md`](c-interop.md#exporting-an-aether-function-as-a-c-callback-c_callback) for the full reference.

### Function-pointer parameters, `fn(T1, T2) -> R`

A parameter typed `fn(T1, T2) -> R` is a typed C function pointer: it emits the exact `R (*name)(T1, T2)` in the generated prototype and is directly callable in the body, so callback-taking functions port cleanly:

```aether,fragment
walk(cb: fn(ptr, ptr) -> void, a: ptr, b: ptr) {
    cb(a, b)                       // a real indirect call
}

reduce(f: fn(int, int) -> int, x: int, y: int) -> int {
    return f(x, y)                 // multiple callbacks per signature are fine
}
```

Pass an Aether function's address with the `as fn(...)` cast, `walk(my_handler as fn(ptr, ptr) -> void, p, q)` or a C function pointer obtained from an extern. This is the parameter form of the same typed-fn-pointer machinery used by `as fn(...)` locals and function-pointer struct fields; the prototype matches the C signature exactly (needed for callback APIs like `qsort`, `dictScan`, signal handlers, libcurl/sqlite hooks).

### `@derive(eq)` synthesize an equality helper for a struct

Annotate a struct definition with `@derive(eq)` and the compiler synthesizes `int <StructName>_eq(<StructName> a, <StructName> b)` automatically, a field-by-field `==` chain that returns `1` when every field matches, `0` otherwise:

```aether
@derive(eq)
struct Point { x: int, y: int }

main() {
    a = Point { x: 1, y: 2 }
    b = Point { x: 1, y: 2 }
    if Point_eq(a, b) == 1 { println("equal") }
}
```

Supported field types in v1: primitive numeric (`int`, `long`, `float`, `byte`, `bool`) and `string`. The codegen lowers `string == string` to `strcmp(...) == 0` automatically, so the synthesizer doesn't need a special path.

`@derive(format)` / `clone` / `hash` and nested-struct fields surface a precise compile-time diagnostic, they're explicitly out of v1 scope and tracked for follow-up commits.


---

## Built-in Functions

### I/O

| Function | Description |
|----------|-------------|
| `print(value)` | Print to stdout (no newline) |
| `println(value)` | Print to stdout followed by a newline |
| `print_char(code)` | Print a single character by ASCII/Unicode code point |

String interpolation is supported inside double-quoted strings using `${expr}`:

```aether,fragment
name = "Alice"
age = 30
println("Hello, ${name}! You are ${age} years old.")
```

Interpolated strings produce a `ptr` (heap-allocated C string) when used as values:

```aether,fragment
msg = "Hello, ${name}!"         // msg is a ptr (char*), not an int
tcp_send_raw(conn, msg)          // can be passed to any function expecting ptr
```

When used directly inside `print`/`println`, the compiler optimizes to a `printf` call (no allocation).

### Heredoc strings

`<<MARKER … MARKER` captures a multi-line **literal** string, no `${}`
interpolation and no escape processing, so it's ideal for embedding another
language's source verbatim (SQL, a `contrib.host.*` snippet) without escaping
the guest's own quotes. The heredoc only triggers when `<<` is immediately
followed by an identifier; `1 << 4` stays the left-shift operator.

```aether,fragment
sql = <<SQL
SELECT id, name
FROM users
WHERE active = 1
SQL
```

The body runs from the line after `<<MARKER` to the **closing-marker line**: a
line that is, in full, optional leading whitespace followed by exactly `MARKER`
and then the end of the line. The closing marker **may be indented**, it does
not have to sit at column 0, but its indentation must be **at or below** the
shallowest body line (the terminator lives at the content's base level). Windows
`\r\n` is normalized to `\n`, and the single newline immediately before the
closing marker is dropped (it's syntax, not content).

Because the terminator sits at the content's base level, a body line that is
*more* indented than the rest but happens to read exactly like the marker is
**body content, not a terminator**, so it can never silently truncate the
string. (Conversely, a lone marker indented *past* the body matches nothing and
the heredoc is reported as unterminated, rather than dropping content. Put the
closing marker at column 0, or aligned with the body's base indent, and it
always closes.) The marker must also be alone on its line: `done MARKER` and
`xMARKER` stay content.

**Common-indent dedent.** The longest run of leading whitespace shared by every
*non-blank* line is stripped, so a heredoc can be indented to match its
surrounding code without that indentation leaking into the string:

```aether,fragment
fn describe() -> string {
    return <<TEXT
        line one
          line two
    TEXT
}
// → "line one\n  line two"   (common 8-space prefix removed; the relative
//                              2-space indent of "line two" is preserved)
```

Rules:
- **Blank / whitespace-only lines don't constrain** the common prefix (a single
  blank line won't force the prefix to zero), but they're still emitted.
- The prefix match is **character-exact**: if one line indents with spaces and
  another with a tab at the same column, that's a disagreement and the strip
  stops there, Aether never shifts past a column where lines differ. To keep a
  literal common indent in the string, indent one line one notch less than the
  rest (so the common prefix is shorter than the indent you want kept).
- A line indented *less* than the common prefix simply loses what leading
  whitespace it has and lands at column 0.

### Timing

| Function | Description |
|----------|-------------|
| `clock_ns()` | Returns current time in nanoseconds (`long`) |
| `sleep(ms)` | Pause execution (milliseconds) |

### Concurrency

| Function | Description |
|----------|-------------|
| `spawn(ActorName())` | Create actor instance |
| `wait_for_idle()` | Wait for all actors to finish |

### Environment & Process

| Function | Description |
|----------|-------------|
| `getenv(name)` | Get environment variable (returns string) |
| `atoi(s)` | Convert string to int |
| `exit(code)` | Terminate program with exit code (defaults to 0) |

---

## Compiler Warnings

The compiler emits structured warnings for common issues:

### Unused Variables [W1001]

Variables declared but never referenced produce a warning. Prefix with `_` to suppress:

```aether
main() {
    x = 42          // warning[W1001]: unused variable 'x'
    _unused = 42    // no warning, intentional discard
    y = 10
    println(y)      // y is used, no warning
}
```

### Unreachable Code [W1002]

Code after `return`, `exit()`, or exhaustive `if`/`else` blocks is flagged:

```aether,fragment
check(x: int) -> {
    if x > 0 { return 1 }
    else { return 0 }
    println("never reached")    // warning[W1002]: unreachable code
}
```

### Constant-Expression Overflow [W1003]

An `int` expression whose operands are all constants is folded at
compile time. When its exact value does not fit 32 bits, the fold wraps
exactly as the runtime would and reports what happened:

```aether
main() {
    cv = (250000000 - 200000000) * 50 / 225000000
    // warning[W1003]: int constant expression overflows 32 bits: the
    // exact value is 2500000000, which wraps to -1794967296 at runtime.
}
```

Folding wraps rather than keeping the wide value on purpose: otherwise
the constant form and the identical expression written over `int`
variables would disagree, and the literal form (the one you reach for
while developing) would look correct while production code silently
overflowed. Widen a term to keep the value:

```aether,fragment
long span = 250000000 - 200000000
cv = span * 50 / 225000000          // 11, no warning
```

Use `ae check file.ae` to see warnings without compiling. It skips codegen and linking, so iteration is much faster than `ae build`.

---

### Unreachable Match Arm [W1004]

`match` tries its arms in source order, so an arm that an earlier arm
already covers can never run. Two shapes are reported: an arm below a
`_` catch-all, and a second arm for a case already handled.

```aether
main() {
    x = 2
    match x {
        1 -> { println("one") }
        2 -> { println("two") }
        2 -> { println("never runs") }
        // warning[W1004]: unreachable match arm: this case is already
        // handled on line 5
        _ -> { println("other") }
    }
}
```

The check reports only where the shadowing is certain. A bare
identifier arm binds the value rather than naming a case, so it matches
anything and is never compared against other arms:

```aether,fragment
match x {
    1 -> { println("one") }
    n -> { println("got ${n}") }   // a binding, not a duplicate: no warning
}
```

A third shape is the mirror of the exhaustiveness check: a `_` arm on a
`match` over a sum type or enum where every case is already handled.

```aether,fragment
enum Direction { North, East, South, West }

match d {
    Direction.North -> { println("N") }
    Direction.East  -> { println("E") }
    Direction.South -> { println("S") }
    Direction.West  -> { println("W") }
    _ -> { println("?") }
    // warning[W1004]: every member of enum 'Direction' is already
    // handled, so the `_` arm cannot run
}
```

Removing the catch-all is worth doing rather than silencing: with it
gone, adding a member later becomes a compile error from the
exhaustiveness check instead of quietly falling into `_`. A `_` over a
partially-handled enum is doing real work and is never reported.

This pairs with the exhaustiveness check, which reports the opposite
direction, a `match` over a sum type or enum that is missing a case.

## Match Expressions

`match` can be used as a statement or as an expression:

```aether,fragment
// Statement, executes the matching arm
match status {
    0 -> println("ok")
    1 -> println("warning")
    _ -> println("error")
}

// Expression, assigns the matching arm's value
msg = match status {
    0 -> "ok"
    1 -> "warning"
    _ -> "error"
}
println(msg)
```

Supported patterns: integer literals, string literals, `_` (wildcard), list patterns.

---

## Keywords

The following identifiers are reserved:

| Keyword | Purpose |
|---------|---------|
| `if`, `else` | Conditionals |
| `while`, `for`, `in`, `break`, `continue` | Loops |
| `return` | Function return |
| `match`, `switch`, `case`, `default` | Pattern matching / dispatch |
| `actor`, `receive`, `spawn`, `reply`, `after` | Actor system |
| `message`, `struct` | Type definitions |
| `state` | Actor state (only reserved inside actor bodies) |
| `import`, `extern` | Modules and C interop |
| `as` | Import aliasing (`import std.string as str`) |
| `const` | Top-level constants |
| `var` | Mutable module-level globals |
| `defer` | Scope-exit cleanup |
| `hide`, `seal`, `except` | Scope-level name denial (see [hide-and-seal.md](hide-and-seal.md)) |
| `null`, `true`, `false` | Literals |
| `when` | Guard clauses |
| `int`, `float`, `string`, `bool`, `ptr`, `long` | Type names |

Note: `state` is context-sensitive, it is a keyword only inside actor bodies. In all other code, `state` can be used as a regular variable name.

Note: **`ptr`, `byte`, `func`, `state` and `after` are usable as ordinary value identifiers**, parameter names, local names, struct field names, struct-literal fields, and field-access targets, *without* the backtick escape. These tokens have meaning only in type position (`ptr`/`byte`), as a declaration head (`func`), or as a statement head (`state`/`after`), none of which overlaps a value position, so a bare `ptr`/`byte`/`func`/`state`/`after` in value position is unambiguously a name:

```aether
add(ptr: int) -> int { return ptr + 1 }     // `ptr` is the parameter name
struct Node { func: int  after: int }        // keyword-spelled field names
main() { let byte = 7  println("${byte}") }  // keyword-spelled local
```

Two members of that family stay reserved in value position: `match` (it heads a match expression, so `match` as a bare value is genuinely ambiguous) and `union` (a C keyword, a value named `union` could not be emitted as valid C). For those, rename or use the backtick escape below. As a type keyword, `byte`/`ptr` still works in type position too: `byte b` declares `b` of type `byte` (the `<type> <name>` C-style form), while `byte: int` / `byte` in name position is the name.

Note: `void` is **not** a reserved word, there is no `void` token. It is a plain identifier used by convention to *spell* the absence of a return value (e.g. `extern free(p: ptr) -> void`); a function that omits its `-> Type` annotation is the canonical void declaration. See [Functions](#functions).

### Raw identifiers, using a keyword as a name

A backtick-delimited identifier, `` `name` ``, is always lexed as an ordinary
identifier, bypassing keyword reservation. This lets a reserved word be used
verbatim wherever an identifier is expected, parameter, local, struct-field,
message-field, or function name:

```aether,fragment
struct Robj {
    `reply`: int        // `reply` is a keyword; the backtick escape keeps it
    `message`: int
}

addReply(`reply`: int, `after`: int) -> int {
    `ptr` = `reply` + `after`
    return `ptr`
}
```

The escaped text is the name (`` `reply` `` is the identifier `reply`), so a
raw identifier and the plain spelling refer to the same binding. The primary
use is keeping a C→Aether port faithful: C APIs routinely use names like
`reply`, `message`, and `when` that collide with Aether keywords.

(For the common port collisions `ptr`, `byte`, `func`, `state` and `after`,
the backtick escape is **not** required, they are accepted bare as value
identifiers, see the note under [Keywords](#keywords) above. The escape is
still the way to use a fully-reserved keyword such as `reply`, `message`,
`when`, `match`, or `union` as a name.)

Writing an *unescaped* reserved keyword where a name is expected is an error
(`error[E0100]: '<kw>' is a reserved keyword …`) whose hint points to both the
rename and the `` `<kw>` `` escape.

Module paths are the exception: a segment there is a name and nothing else, so
`import std.message` needs no escape. Calling through the bare module name
still would (`message.format(...)` puts a keyword in expression position), so
import the symbols you need by name:

```aether,fragment
import std.message (format, catalog_new)   // format(...), catalog_new(...)
```

(`import std.message as msg` parses, but qualified calls through an alias do
not resolve yet, see [Import with Alias](#import-with-alias).)

---

## Comments

```aether,fragment
// Single-line comment

/* Multi-line
   comment */
```

---

## Conditional Compilation

Two axes: the platform, which the C compiler knows, and **build symbols**,
which the build declares. Both are resolved at compile time.

### Build symbols

A symbol is present or absent. It carries no value.

```sh
ae build -D TEST_SERVER app.ae      # or -DTEST_SERVER
aetherc -D TEST_SERVER app.ae
```

```toml
# aether.toml, for symbols the project always wants
[build]
defines = "TEST_SERVER VERBOSE"
```

A command-line `-D` adds to what the file declares. A symbol must be an
identifier: letters, digits and underscore, not starting with a digit. `-D
NAME=value` is rejected rather than accepted and ignored, because a symbol has
no value to carry.

### `when` regions

`when defined(NAME) { … }` guards a group of declarations or a group of
statements, with an optional `else`:

```aether
when defined(TEST_SERVER) {
    driver_port() -> int { return 9222 }
    start_driver() { println("driver on ${driver_port()}") }
} else {
    start_driver() { println("driver absent") }
}

main() {
    start_driver()
    when defined(VERBOSE) {
        println("verbose")
    }
}
```

Conditions combine with `!`, `&&`, `||` and parentheses:

```aether,fragment
when defined(A) && !defined(B) { … }
when (defined(A) || defined(B)) && !defined(C) { … }
```

**A losing region is dropped, not disabled.** It is removed from the program
before type checking, so nothing it names has to exist, and its symbols are not
in the binary: `nm` on a build without `TEST_SERVER` finds no `driver_port`.
That is the difference between this and wrapping code in a runtime `if`, which
still compiles and links every line of the subsystem you meant to leave out.

**A dropped `import` takes its native dependencies with it.** The effect is
not limited to your own symbols. A module declares its native libraries with
`@link`, and those flags reach the linker only when the module is in the
resolved import closure — so guarding the `import` removes the C library too,
transitively and at any depth:

```aether,fragment
when defined(WITH_SQLITE) {
    import contrib.sqlite
    ...
}
```

Built without `WITH_SQLITE`, the binary does not link `libsqlite3` at all —
`ldd` does not list it. That is usually the reason to reach for `when` in the
first place: the measured difference on this example is 4.4x. See
`docs/module-system-design.md` for how `@link` propagates.

The corollary is that a disabled region is not type-checked. Code you never
build is code you never check, so a symbol that is off in every CI
configuration will rot. Build each configuration you ship.

### `select` for values

`select` chooses a value rather than a region. Its keys are the platforms plus
any build symbol:

```aether,fragment
port = select(DEV_MODE: 8080, other: 80)
tag  = select(linux: "linux", macos: "macos", windows: "windows", other: "other")
```

A build symbol wins outright when it is set, since it is known at compile time;
otherwise the platform decides, and `other:` is the fallback. Every branch must
yield the same type, and a mismatch is an error rather than a surprise on
whichever platform compiles the odd branch.

Use `select` for a value and `when` for anything larger: `select` cannot remove
a declaration, and both of its branches still compile.

**If you are trying to keep something out of the binary, `select` is the wrong
tool.** It picks between values that both survive to codegen; it cannot guard
an `import` or a declaration, so it never removes a symbol or a native
dependency. `when` is the construct that does that.

## Compilation

### Using the CLI

```bash
ae run program.ae           # Compile and run (fast, -O0)
ae build program.ae -o out  # Compile to optimised executable (-O2 + aether.toml cflags)
ae init myproject           # Scaffold a new project
ae test                     # Discover and run .ae test files
ae cache                    # Show build cache info
ae cache clear              # Purge build cache
```

`ae run` and `ae build` also accept:

```bash
# Include extra C source files (e.g. FFI helpers, renderer backends)
ae build main.ae -o app --extra src/ffi.c --extra src/renderer.c

# Multiple --extra flags are additive; also merged with extra_sources from aether.toml
```

### Using the Compiler Directly

```bash
# Compile to C
aetherc program.ae output.c

# Emit a C header for embedding Aether actors in a C application
# Generates message structs, MSG_* constants, and spawn function prototypes
aetherc program.ae output.c --emit-header

# Print parsed AST (for debugging, no code generation)
aetherc --dump-ast program.ae
```

---

## Type System

Aether uses static typing with full type inference, explicit annotations are never required, but are always accepted.

### Inference rules

- **Local variables**: Inferred from their initializer (`x = 42` → `int`)
- **Function parameters**: Inferred from call sites across the whole program, including through deep call chains (`main → f → g → h`)
- **Return types**: Inferred from `return` statements and arrow-body expressions
- **Constraint solving**: Iterative constraint propagation handles complex interdependencies

### Type annotations are optional

```aether,fragment
// All three are equivalent:
add(a, b) { return a + b; }          // fully inferred from call sites
add(a: int, b: int) { return a + b; } // explicit
add(a, b: int) { return a + b; }     // mixed
```

Annotations are useful for documentation or when the type cannot be determined from call sites alone (e.g. a function that is never called, or an `extern` parameter).

### `extern` requires annotations

The compiler cannot infer types of external C functions, parameter types must be declared explicitly:

```aether,fragment
extern malloc(n: int) -> ptr
extern free(p: ptr) -> void
```

Explicit types are optional but can improve clarity:

```aether,fragment
// Both are valid:
x = 42;
int y = 42;
```

---

## Example Programs

### Hello World

```aether
main() {
    print("Hello, World!\n");
}
```

### Factorial with Pattern Matching

```aether
factorial(0) -> 1
factorial(n) when n > 0 -> n * factorial(n - 1)

main() {
    print(factorial(10))  // 3628800
}
```

### Counter Actor

```aether
message Increment { amount: int }
message GetCount {}

actor Counter {
    state count = 0;

    receive {
        Increment(amount) -> {
            count = count + amount;
        }
        GetCount() -> {
            print(count);
            print("\n");
        }
    }
}

main() {
    c = spawn(Counter());
    c ! Increment { amount: 5 };
    c ! Increment { amount: 3 };
    c ! GetCount {};
    wait_for_idle();  // Output: 8
}
```

### Resource Management with Defer

```aether,fragment
extern fopen(path: string, mode: string) -> ptr
extern fclose(file: ptr) -> int

process_file(path) {
    file = fopen(path, "r");
    defer fclose(file);

    // Process file...
    // fclose called automatically
}
```
