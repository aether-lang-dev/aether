# Aether Language Tutorial

This tutorial walks through Aether's core features with working examples you can run. Aether is a compiled language built around the **actor model**, lightweight concurrent entities that communicate through messages.

Run any example with:
```bash
ae run myfile.ae
```

## Table of Contents
1. [Hello World](#1-hello-world)
2. [Your First Actor](#2-your-first-actor)
3. [Actor State & Multiple Messages](#3-actor-state--multiple-messages)
4. [Control Flow](#4-control-flow)
5. [Pattern Matching](#5-pattern-matching)
6. [Multiple Actors](#6-multiple-actors)
7. [String Interpolation](#7-string-interpolation)
8. [Constants, Null, and Bitwise](#8-constants-null-and-bitwise)
9. [Ergonomic Syntax](#9-ergonomic-syntax)

---

## 1. Hello World

```aether
main() {
    println("Hello, Aether!")

    x = 42
    y = 8

    if (x > y) {
        println("x is greater than y")
    }

    sum = x + y
    println("Sum: ${sum}")
}
```

**What you see:**
- `main()` is the entry point
- Variables use **type inference**, no explicit types needed
- `println` prints with a newline; `print` prints without
- `${variable}` embeds a value into a string

---

## 2. Your First Actor

Actors are the core abstraction in Aether, lightweight concurrent entities that process messages.

```aether
message Ping {}

actor Counter {
    state count = 0

    receive {
        Ping() -> {
            count = count + 1
            println("Count: ${count}")
        }
    }
}

main() {
    c = spawn(Counter())
    c ! Ping {}
    c ! Ping {}
    c ! Ping {}

    wait_for_idle()
}
```

**Output:**
```
Count: 1
Count: 2
Count: 3
```

**Key concepts:**
- `message Ping {}` defines a message type (empty here, but can have fields)
- `actor Counter { state ... receive { ... } }` an actor with private state
- `spawn(Counter())` creates a Counter actor; returns a handle
- `c ! Ping {}` sends an asynchronous message
- `wait_for_idle()` blocks until all actors have finished processing

Without `wait_for_idle()`, main could exit before the actor processes its messages.

---

## 3. Actor State & Multiple Messages

Actors can handle multiple message types and carry state across messages:

```aether
message Increment { amount: int }
message Decrement { amount: int }
message Reset {}
message GetValue {}

actor Counter {
    state count = 0

    receive {
        Increment(amount) -> {
            count = count + amount
        }
        Decrement(amount) -> {
            count = count - amount
        }
        Reset() -> {
            count = 0
        }
        GetValue() -> {
            println("Current value: ${count}")
        }
    }
}

main() {
    c = spawn(Counter())
    c ! Increment { amount: 10 }
    c ! Increment { amount: 5 }
    c ! GetValue {}
    c ! Decrement { amount: 3 }
    c ! GetValue {}
    c ! Reset {}
    c ! GetValue {}

    wait_for_idle()
}
```

**Output:**
```
Current value: 15
Current value: 12
Current value: 0
```

Messages are processed **in order** within a single actor. State (`count`) is private, nothing outside the actor can access it directly.

---

## 4. Control Flow

Aether has standard control structures:

```aether
main() {
    // For loops (with compound assignment operators)
    sum = 0
    for (i = 1; i <= 10; i++) {
        sum += i
    }
    println("Sum 1..10 = ${sum}")

    // While loops
    n = 16
    steps = 0
    while (n > 1) {
        if (n % 2 == 0) {
            n = n / 2
        } else {
            n = n * 3 + 1
        }
        steps = steps + 1
    }
    println("Collatz steps for 16: ${steps}")

    // If / else if / else
    score = 85
    if (score >= 90) {
        println("Grade: A")
    } else if (score >= 80) {
        println("Grade: B")
    } else {
        println("Grade: C or below")
    }
}
```

**Output:**
```
Sum 1..10 = 55
Collatz steps for 16: 4
Grade: B
```

---

## 5. Pattern Matching

Pattern matching, inspired by Erlang, lets you dispatch on argument values and structure.

### Function Clauses

Define functions with multiple clauses that match on argument values:

```aether
// Recursive factorial with pattern matching
factorial(0) -> 1
factorial(n) when n > 0 -> n * factorial(n - 1)

// Fibonacci
fib(0) -> 0
fib(1) -> 1
fib(n) when n > 1 -> fib(n - 1) + fib(n - 2)

// Classify a number
classify(x) when x < 0 -> "negative"
classify(x) when x == 0 -> "zero"
classify(x) when x > 0 -> "positive"

main() {
    println("5! = ${factorial(5)}")
    println("fib(8) = ${fib(8)}")
    println("classify(-3) = ${classify(-3)}")
    println("classify(0) = ${classify(0)}")
    println("classify(7) = ${classify(7)}")
}
```

**Output:**
```
5! = 120
fib(8) = 21
classify(-3) = negative
classify(0) = zero
classify(7) = positive
```

### Match Statements

For value dispatch inside functions or message handlers:

```aether
message Process { code: int }

actor Dispatcher {
    receive {
        Process(code) -> {
            match (code) {
                0 -> { println("OK") }
                1 -> { println("Warning") }
                2 -> { println("Error") }
                _ -> { println("Unknown code: ${code}") }
            }
        }
    }
}

main() {
    d = spawn(Dispatcher())
    d ! Process { code: 0 }
    d ! Process { code: 2 }
    d ! Process { code: 99 }
    wait_for_idle()
}
```

**Output:**
```
OK
Error
Unknown code: 99
```

---

## 6. Multiple Actors

Actors can be spawned in any number and run concurrently across all CPU cores:

```aether
message Work { id: int, value: int }
message Done {}

actor Worker {
    state processed = 0

    receive {
        Work(id, value) -> {
            result = value * value
            println("Worker ${id}: ${value}^2 = ${result}")
            processed = processed + 1
        }
        Done() -> {
            println("Worker finished ${processed} tasks")
        }
    }
}

main() {
    w1 = spawn(Worker())
    w2 = spawn(Worker())
    w3 = spawn(Worker())

    // Distribute work across workers
    w1 ! Work { id: 1, value: 3 }
    w2 ! Work { id: 2, value: 5 }
    w3 ! Work { id: 3, value: 7 }
    w1 ! Work { id: 1, value: 9 }
    w2 ! Work { id: 2, value: 11 }

    w1 ! Done {}
    w2 ! Done {}
    w3 ! Done {}

    wait_for_idle()
}
```

The Aether runtime automatically distributes actors across CPU cores. Message ordering is guaranteed **within** a single actor, but messages to different actors may be processed in any order.

---

## 7. String Interpolation

Use `${expression}` to embed values in strings:

```aether
main() {
    name = "Aether"
    version = 1
    pi = 3

    println("Welcome to ${name} v${version}!")
    println("Pi is approximately ${pi}")

    for (i = 1; i <= 5; i = i + 1) {
        squared = i * i
        println("${i}^2 = ${squared}")
    }
}
```

String interpolation works with `println` and any function that accepts a string. Variables of any type (int, string) can be interpolated.

Interpolated strings produce a real string value when assigned to a variable, so they can be passed to any function:

```aether,fragment
msg = "Hello, ${name}!"     // msg is a string pointer
```

---

## 8. Constants, Null, and Bitwise

### Constants

Define named values at the top level with `const`:

```aether
const WIDTH = 80
const HEIGHT = 24
const TITLE = "My App"

main() {
    println(TITLE)
    area = WIDTH * HEIGHT
    println("Area: ${area}")
}
```

### Null

Use `null` for uninitialized pointers:

```aether
main() {
    conn = null
    if conn == null {
        println("Not connected")
    }
}
```

### Bitwise Operators

Aether supports `&`, `|`, `^`, `~`, `<<`, `>>` for bit manipulation:

```aether
main() {
    flags = 5 & 3         // AND: 1
    mask = 5 | 3          // OR: 7
    flipped = 5 ^ 3       // XOR: 6
    shifted = 1 << 4      // Left shift: 16
    println("${flags} ${mask} ${flipped} ${shifted}")
}
```

---

## 9. Ergonomic Syntax

Aether includes several features that reduce boilerplate.

### Hex, Octal, and Binary Literals

```aether
main() {
    flags = 0xFF
    mask = 0x0F
    bits = 0b1010_0101
    perms = 0o755

    println(flags & mask)    // 15
    println(bits >> 4)       // 10
}
```

### If-Expressions

Use `if`/`else` as a value-producing expression:

```aether
main() {
    x = 10
    sign = if x > 0 { 1 } else { -1 }
    println(sign)   // 1
}
```

### Range-Based For Loops

```aether
main() {
    // Instead of: for (i = 0; i < 10; i++) { ... }
    for i in 0..10 {
        print(i)
        print(" ")
    }
    // prints: 0 1 2 3 4 5 6 7 8 9
}
```

### Multi-Statement Arrow Bodies

Arrow functions can contain multiple statements. The last expression is the implicit return:

```aether
distance(x1, y1, x2, y2) -> {
    dx = x2 - x1
    dy = y2 - y1
    dx * dx + dy * dy
}

main() {
    println(distance(0, 0, 3, 4))  // 25
}
```

---

## Next Steps

### Best Practices

1. **One responsibility per actor**, keep actors focused on a single task
2. **No shared state**, communicate only through messages, never share memory
3. **Always `wait_for_idle()`** when you need to read actor state or coordinate completion
4. **Descriptive message names**, `Increment` is clearer than `Update`

### Further Reading

- [Getting Started](getting-started.md), Installation and project setup
- [Language Reference](language-reference.md), Complete syntax specification
- [Standard Library](stdlib-reference.md), Collections, I/O, networking
- [Architecture](architecture.md), Compiler and runtime internals
- [Runtime Optimizations](runtime-optimizations.md), Performance details

---

## Summary

| Feature | Syntax |
|---------|--------|
| Define message | `message Name { field: type }` |
| Define actor | `actor Name { state x = 0; receive { ... } }` |
| Spawn actor | `a = spawn(ActorName())` |
| Send message | `a ! MessageName { field: value }` |
| Wait for all | `wait_for_idle()` |
| Pattern clause | `fn(0) -> result` |
| Guard | `fn(n) when n > 0 -> ...` |
| Match | `match (x) { 0 -> {...} _ -> {...} }` |
| Interpolation | `"text ${variable}"` |
| Constant | `const NAME = value` |
| Null | `x = null` |
| Bitwise | `&` `\|` `^` `~` `<<` `>>` |
| Compound assign | `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` |
| Hex/bin/oct | `0xFF`, `0b1010`, `0o755` |
| If-expression | `if cond { a } else { b }` |
| Range for | `for i in 0..10 { ... }` |
| Arrow block | `f(x) -> { stmts; expr }` |
