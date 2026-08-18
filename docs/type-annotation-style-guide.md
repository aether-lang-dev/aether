# Aether Type Annotation Style Guide

## Philosophy

Aether supports **both type inference and explicit type annotations**, giving you flexibility to choose the right approach for your code. This guide provides best practices for when to use each.

## Type Inference (Recommended Default)

Aether's type inference can deduce types automatically in most cases.

### When to Use Type Inference

**Local variables**
```aether,fragment
// Type inference - clean and concise
x = 42              // Inferred as int
name = "Alice"      // Inferred as string
points = [1, 2, 3]  // Inferred as int array
```

**Simple computations**
```aether,fragment
sum = a + b
product = x * y
result = compute_value()
```

**Loop variables**
```aether,fragment
for i in 0..10 {
    total = total + i
}
```

**Struct field access**
```aether,fragment
point = Point{ x: 5, y: 10 }
x_value = point.x  // Inferred from struct definition
```

## Explicit Types (For Clarity)

Use explicit type annotations when they improve code clarity or are required.

### When to Use Explicit Types

**Function signatures** (highly recommended)
```aether,fragment
// Function parameters and return types
add(int a, int b): int {
    return a + b
}

greet(string name): void {
    println("Hello, ${name}")
}

find_user(int id): User {
    // ... implementation
}
```

**Why**: Makes function contracts clear, improves documentation, helps catch errors at call sites.

**Public APIs and library functions**
```aether,fragment
// Export functions should have explicit types
import std.math

export calculate_distance(p1: Point, p2: Point): float {
    dx = p2.x - p1.x
    dy = p2.y - p1.y
    return math.sqrt(dx * dx + dy * dy)
}
```

**Complex types or ambiguous cases**
```aether,fragment
import std.map
import std.list

// map.new() / list.new() return an opaque ptr; let inference bind it
my_map = map.new()
users = list.new()

// Explicit type prevents ambiguity
int result = parse_number(input)  // Clarifies we want int, not float
```

**Actor state declarations**
```aether,fragment
actor Counter {
    state int count            // Explicit prefix form for clarity
    state string name
    state int[] values
    state total = 0            // ...or inferred from an initializer
    state users = null         //    (inferred: ptr)
    
    // ...
}
```

**Struct field definitions**
```aether,fragment
struct User {
    int id
    string name
    string email
    bool active
}
```

## Style Consistency

### Within a Module

**Be consistent** within a single file or module:

```aether,fragment
// Option A: Pure inference (for small scripts)
x = 10
y = 20
sum = x + y

// Option B: Explicit types (for larger projects)
int x = 10
int y = 20
int sum = x + y
```

**Don't mix** styles randomly:
```aether,fragment
// Inconsistent - avoid
int x = 10
y = 20           // Sudden switch to inference
int sum = x + y  // Back to explicit
```

### Across a Project

Establish team conventions:
- **Scripts/prototypes**: Use type inference for speed
- **Libraries/APIs**: Use explicit types for clarity
- **Performance-critical code**: Use explicit types for control

## Type Annotations Syntax

### Variables

```aether,fragment
// Explicit type annotation
int x = 42
string name = "Alice"
float pi = 3.14159
bool active = true

// Type inference (no annotation)
x = 42
name = "Alice"
pi = 3.14159
active = true
```

### Functions

```aether,fragment
// Explicit parameter and return types
multiply(int a, int b): int {
    return a * b
}

// Mixed: explicit parameters, inferred return
add(int a, int b) {
    return a + b  // Return type inferred
}

// Full inference (not recommended for public functions)
subtract(a, b) {
    return a - b
}
```

### Arrays

```aether,fragment
// Type inference from elements (the working form for array literals)
numbers = [1, 2, 3, 4, 5]  // Inferred as int[]
names = ["Alice", "Bob"]   // Inferred as string[]

// const binding, also inferred from the literal
const primes[] = [2, 3, 5, 7]
```

> **Note:** Bind an array literal with inference (`numbers = [...]`) or a
> `const name[] = [...]` declaration. The explicit prefix-type form
> `int[] numbers = [...]` does not compile: codegen lowers the annotation to
> a scalar pointer and the literal spills as excess initializers. The prefix
> array syntax `[T]` is not accepted either.

### Complex Types

```aether,fragment
import std.map
import std.list

// map/list constructors return an opaque ptr, inference is the norm
user_map = map.new()
points = list.new()

// Same pattern for other constructors
my_map = map.new()
my_points = list.new()
counter = spawn(Counter())
```

## Examples

### Example 1: Script-Style (Inference-Heavy)

```aether
// Quick prototype or script
fibonacci(n) {
    if n <= 1 {
        return n
    }
    return fibonacci(n - 1) + fibonacci(n - 2)
}

main() {
    result = fibonacci(10)
    println("Fib(10) = ${result}")
}
```

### Example 2: Library-Style (Explicit Types)

```aether,fragment
// Public library with clear contracts
import std.math

export struct Point {
    float x
    float y
}

export distance(p1: Point, p2: Point): float {
    float dx = p2.x - p1.x
    float dy = p2.y - p1.y
    return math.sqrt(dx * dx + dy * dy)
}

export midpoint(p1: Point, p2: Point): Point {
    return Point{
        x: (p1.x + p2.x) / 2.0,
        y: (p1.y + p2.y) / 2.0
    }
}
```

### Example 3: Balanced Approach (Recommended)

```aether,fragment
// Explicit where it matters, inference elsewhere
import std.file

process_file(string filename): int {
    int count = 0

    // Inference for simple local variables.
    // file.read opens, reads, and closes the path, returning (content, err).
    content, err = file.read(filename)
    if err != "" {
        return count
    }

    println("Read file: ${filename}")
    println(content)

    return count
}
```

## When explicit types are required

Some cases don't give the inferencer enough to work with; annotate explicitly:

```aether,fragment
// This fails - type can't be inferred
x = undefined()
y = get_value()  // If get_value() return type is unknown

// Solution: Provide explicit type
int x = undefined()
int y = get_value()
```

```aether,fragment
// Recursive functions need return type
fibonacci(n) {
    if n <= 1 { return n }
    return fibonacci(n - 1) + fibonacci(n - 2)  // ERROR
}

// Explicit return type
fibonacci(int n): int {
    if n <= 1 { return n }
    return fibonacci(n - 1) + fibonacci(n - 2)
}
```

## Summary

| Use Case | Recommendation | Example |
|----------|---------------|---------|
| Local variables | Inference | `x = 42` |
| Function parameters | Explicit | `add(int a, int b)` |
| Function returns | Explicit | `add(...): int` |
| Struct fields | Explicit | `struct User { int id }` |
| Actor state | Explicit (prefix) | `state int count` |
| Loop variables | Inference | `for i in 0..10` |
| Complex types | Inference | `my_map = map.new()` |
| Public APIs | Explicit | All types annotated |
| Scripts/prototypes | Inference | Minimal annotations |

**Golden Rule**: Use explicit types when they add **clarity** or are **required**. Use inference when types are **obvious** from context.

## Tooling Support

The Aether LSP provides:
- Type inference on hover
- Type error diagnostics
- Auto-completion for type annotations (planned)
- "Add type annotation" quick fix (planned)

---

*For more information on type inference, see [type-inference-guide.md](type-inference-guide.md)*
