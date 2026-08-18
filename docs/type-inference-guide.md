# Aether Type Inference Guide

## How Type Inference Works

Aether implements a type inference system that automatically deduces types from context, reducing the need for explicit type annotations in most cases.

## Basic Inference

### From Literals

```aether,fragment
x = 42              // inferred: int
pi = 3.14           // inferred: float
name = "Alice"      // inferred: string
flag = true         // inferred: bool
p = null            // inferred: ptr
```

A bare integer literal always infers as `int` there is no `long`
literal or `long <value>` prefix. To get a `long` (64-bit) binding,
annotate the declaration explicitly:

```aether,fragment
big = 0             // inferred: int
long wide = 0       // explicit: long (64-bit)
```

### From Expressions

```aether,fragment
a = 10
b = 20
sum = a + b           // inferred: int (both operands are int)

x = 3.14
y = 2.0
result = x * y        // inferred: float
```

### From Functions

```aether
// Return type inferred from return statement
add(a, b) {
    return a + b      // monomorphizes to one signature from the first
                      // inferred call site (not per-call polymorphism)
}

// Parameters inferred from usage
multiply(x, y) {
    return x * y
}

main() {
    n = add(10, 20)             // add inferred as: int -> int -> int
    f = multiply(3.14, 2.0)     // multiply inferred as: float -> float -> float
}
```

## Advanced Inference

### Struct Fields

```aether
struct Point {
    x,    // Type inferred from initialization
    y
}

struct Player {
    name,
    health,
    score
}

main() {
    p = Point{ x: 10, y: 20 }   // x, y inferred as int

    player = Player{
        name: "Alice",          // string
        health: 100,            // int
        score: 0                // int
    }
}
```

Struct fields infer their types from the initialisation site. A struct
declared with bare field names (no `: type`) and never initialised keeps
the fields at the parser's fallback type. Always initialise via a struct
literal at least once, or annotate the field explicitly.

### Array Elements

```aether,fragment
nums = [1, 2, 3, 4, 5]             // inferred: int[]
names = ["Alice", "Bob"]           // inferred: string[]
mixed_ok = [1.0, 2.0, 3.0]         // inferred: float[]
```

### Through Assignments

```aether,fragment
x = 42            // x: int
y = x             // y: int (inferred from x)
z = y + 10        // z: int (inferred from y + int)
```

## When to Use Explicit Types

While inference handles most cases, explicit types are useful for:

### 1. Function Signatures (Documentation)

```aether,fragment
// Explicit types make intent clear
calculate_damage(base: int, defense: int): int {
    return base - defense
}

// vs inferred (less clear to reader)
calculate_damage(base, defense) {
    return base - defense
}
```

### 2. Complex Scenarios

```aether,fragment
// When inference might be ambiguous
process_data(input: float[]) {
    // Explicit type ensures correct interpretation
}
```

### 3. Public APIs

```aether,fragment
// Export with explicit types for clarity
export calculate_score(kills: int, deaths: int, assists: int): float {
    return (kills + assists / 2.0) / (deaths + 1)
}
```

## Mixed Explicit/Inferred

You can mix explicit and inferred types:

```aether,fragment
// Some parameters explicit, some inferred
process(data, threshold: int) {
    return data > threshold   // data type inferred from usage
}

// Explicit return, inferred parameters
format_name(first, last): string {
    return "${first} ${last}"  // parameters inferred as string
}
```

## How Inference Works Internally

### Phase 1: Constraint Collection

The compiler walks the AST and collects type constraints:

```aether,fragment
x = 42
// Constraint: x must be int (from literal 42)

y = x + 10
// Constraint: y must be int (from x:int + 10:int)
```

### Phase 2: Constraint Solving

Constraints are propagated iteratively until all types are known:

```
Iteration 1: x = int (from literal)
Iteration 2: y = int (from x + int)
Done: All types resolved
```

### Phase 3: Validation

Once inferred, types are validated for consistency:

```aether,fragment
x = 42
x = "hello"   // rejected at the C-compile stage (-Wint-conversion),
              // not by an Aether type-inference diagnostic
```

## Null and Pointer Inference

The `null` keyword is typed as `ptr`:

```aether,fragment
conn = null                  // inferred: ptr
conn = tcp_connect_raw(...)  // still ptr, type is consistent
```

Integer `0` interoperates with `ptr` only in limited cases. Assigning `0`
to a variable that is already `ptr`-typed is fine (a null slot). But
`0`-first-then-`ptr` widening is not a general rule: the pass in
`compiler/analysis/type_inference.c` (`widen_ptr_assigned_locals`) only
retypes a `0`-initialized local as `ptr` when that local lives inside a
`ptr`-returning function. The same pattern in a plain `int`-returning
`main()` keeps the local as `int` and fails to compile with
`incompatible pointer to integer conversion`:

```aether,fragment
listen(): ptr {
    server = 0                   // widened to ptr: this function returns ptr
    server = tcp_listen_raw(80)  // OK
    return server
}
```

### Constants

Top-level `const` declarations infer their type from the value:

```aether,fragment
const MAX = 100          // inferred: int
const NAME = "hello"     // inferred: string
```

## Edge Cases

### Ambiguous Types

A local declaration uses C-style prefix syntax (`type name = value`), not
the postfix `name: type` form. A bare `x: int` local does not parse; it
reports `error[E0100]: Expected statement in block`. Postfix `name: type`
is valid only for function parameters and struct fields.

```aether,fragment
// Give the local an explicit type up front
int x = get_value()   // OK - x is int
```

### Generic Functions

```aether,fragment
// Currently not supported - specify explicit types
identity(x: int): int {
    return x
}

// Future: Generic type parameters
// identity<T>(x: T): T {
//     return x
// }
```

## Performance Impact

Type inference happens at **compile-time only**:

- Zero runtime overhead
- Same generated C code as explicit types
- Full type safety maintained
- No performance difference

## Best Practices

### Do:
- Let inference handle obvious cases (literals, simple expressions)
- Use explicit types for public APIs and exports
- Use explicit types when it improves readability
- Mix explicit/inferred for balance

### Don't:
- Over-annotate everything (defeats the purpose)
- Rely on inference for complex logic (document with types)
- Leave ambiguous types un-annotated

## Examples

### Good: Minimal but Clear

```aether
struct Player {
    name: string,    // Explicit for documentation
    health,          // Inferred from init
    score            // Inferred from init
}

calculate_total(base, bonus) {  // Inferred from usage
    return base + bonus
}

main() {
    p = Player{ name: "Alice", health: 100, score: 0 }
    total = calculate_total(p.score, 10)
}
```

### Better: Strategic Explicit Types

```aether
struct Player {
    name: string,
    health: int,     // Explicit: important field
    score: int       // Explicit: important field
}

// Explicit return: public function
calculate_total(base, bonus): int {
    return base + bonus
}

main() {
    p = Player{ name: "Alice", health: 100, score: 0 }
    total = calculate_total(p.score, 10)
}
```

## Error Messages

Compiler diagnostics are E-coded (for example `E0100`, `E0301`) and print
with a source span and a `help:` line. There is no free-form "Type
inference failed" message. For instance, a call to a name the compiler
cannot resolve reports:

```
error[E0301]: Undefined function 'unknown_value'
```

When you do need to pin a local's type, use the C-style prefix form
`int x = ...`; the postfix `x: int = ...` form does not parse.

## Summary

- Inference covers literals, simple expressions, and single-call-site functions.
- Explicit types double as documentation; use them for public APIs and exports.
- Inference is compile-time only, with no runtime cost.
- Inferred types carry the same guarantees as explicit ones.

Type inference removes most annotations while keeping the generated C strictly typed.

