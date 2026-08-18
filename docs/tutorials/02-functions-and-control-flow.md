# Tutorial 2: Functions and Control Flow

**Time:** 1 hour  
**Prerequisites:** Tutorial 1  
**Goal:** Master functions, conditionals, and loops

## Functions

Functions let you organize and reuse code.

### Basic Function

```aether
greet(name) {
    print("Hello, ")
    print(name)
    print("!\n")
}

main() {
    greet("Alice")
    greet("Bob")
}
```

**Output:**
```
Hello, Alice!
Hello, Bob!
```

### Functions with Return Values

```aether
add(a, b) {
    return a + b
}

multiply(x, y) {
    return x * y
}

main() {
    sum = add(10, 20)
    product = multiply(5, 6)
    
    print("Sum: ")
    print(sum)
    print("\n")
    
    print("Product: ")
    print(product)
    print("\n")
}
```

### Type Inference in Functions

Aether infers types from how you use the function:

```aether
// Parameters and return type inferred!
square(n) {
    return n * n
}

main() {
    result = square(5)    // Inferred as: int -> int
    print(result)          // 25
    print("\n")
}
```

### Recursive Functions

```aether
factorial(n) {
    if (n <= 1) {
        return 1
    }
    return n * factorial(n - 1)
}

main() {
    result = factorial(5)  // 120
    print("5! = ")
    print(result)
    print("\n")
}
```

## Control Flow

### If Statements

```aether
check_age(age) {
    if (age >= 18) {
        print("Adult\n")
    } else {
        print("Minor\n")
    }
}

main() {
    check_age(25)  // Adult
    check_age(15)  // Minor
}
```

### Comparison Operators

```aether
main() {
    x = 10
    y = 20
    
    if (x == y) { print("Equal\n") }
    if (x != y) { print("Not equal\n") }
    if (x < y) { print("x is less\n") }
    if (x > y) { print("x is greater\n") }
    if (x <= y) { print("x is less or equal\n") }
    if (x >= y) { print("x is greater or equal\n") }
}
```

### Nested If

```aether
classify_number(n) {
    if (n > 0) {
        if (n > 100) {
            print("Large positive\n")
        } else {
            print("Small positive\n")
        }
    } else {
        if (n < -100) {
            print("Large negative\n")
        } else {
            print("Small negative or zero\n")
        }
    }
}

main() {
    classify_number(150)   // Large positive
    classify_number(50)    // Small positive
    classify_number(-150)  // Large negative
    classify_number(-5)    // Small negative or zero
}
```

## Loops

### While Loop

```aether
main() {
    i = 0
    while (i < 5) {
        print(i)
        print(" ")
        i = i + 1
    }
    print("\n")
}
```

**Output:** `0 1 2 3 4`

### For Loop

```aether
main() {
    for (i = 0; i < 5; i = i + 1) {
        print(i)
        print(" ")
    }
    print("\n")
}
```

**Output:** `0 1 2 3 4`

### Counting Down

```aether
main() {
    for (i = 5; i > 0; i = i - 1) {
        print(i)
        print(" ")
    }
    print("Liftoff!\n")
}
```

**Output:** `5 4 3 2 1 Liftoff!`

## Practical Examples

### Example 1: Find Maximum

```aether
max(a, b) {
    if (a > b) {
        return a
    } else {
        return b
    }
}

main() {
    x = 42
    y = 17
    
    biggest = max(x, y)
    print("Maximum: ")
    print(biggest)
    print("\n")
}
```

### Example 2: Sum of Numbers

```aether
sum_to_n(n) {
    total = 0
    for (i = 1; i <= n; i = i + 1) {
        total = total + i
    }
    return total
}

main() {
    result = sum_to_n(10)  // 1+2+3+...+10 = 55
    print("Sum 1 to 10: ")
    print(result)
    print("\n")
}
```

### Example 3: Fibonacci Sequence

```aether
fibonacci(n) {
    if (n <= 1) {
        return n
    }
    return fibonacci(n - 1) + fibonacci(n - 2)
}

main() {
    print("First 10 Fibonacci numbers:\n")
    for (i = 0; i < 10; i = i + 1) {
        print(fibonacci(i))
        print(" ")
    }
    print("\n")
}
```

**Output:** `0 1 1 2 3 5 8 13 21 34`

## Functional Style: Pattern Matching + Recursion

Aether is inspired by Erlang and supports a functional programming style using pattern matching and recursion. This can often lead to cleaner, more expressive code.

### Two Ways to Solve Problems

Here are several examples showing the same problem solved both ways:

#### Sum of List

**Imperative style (for loop):**
```aether,fragment
sum_loop(nums, len) {
    total = 0
    for (i = 0; i < len; i = i + 1) {
        total = total + nums[i]
    }
    return total
}
```

**Functional style (recursion + pattern matching):**
```aether
import std.string

// A *StringSeq is a cons-cell list (head + tail). `[]` matches the
// empty list; `[h | t]` binds the head `h` and the tail `t`, so the
// function can recurse on the tail.
total_length(words: *StringSeq) {
    match words {
        []      -> { return 0 }
        [h | t] -> { return string.length(h) + total_length(t) }
    }
    return 0
}

main() {
    words = string.seq_empty()
    words = string.seq_cons("hi", words)
    words = string.seq_cons("hello", words)
    words = string.seq_cons("hey", words)
    println(total_length(words))  // 10
    string.seq_free(words)
}
```

The functional style reads like a definition: "The total length of an empty list is 0; the total length of a list is the length of the head plus the total length of the tail."

#### Factorial

**Imperative style:**
```aether,fragment
factorial_loop(n) {
    result = 1
    for (i = 2; i <= n; i = i + 1) {
        result = result * i
    }
    return result
}
```

**Functional style:**
```aether,fragment
factorial(n) {
    if (n <= 1) { return 1 }
    return n * factorial(n - 1)
}
```

#### Finding Maximum in List

**Imperative style:**
```aether,fragment
max_loop(nums, len) {
    if (len == 0) { return 0 }
    max_val = nums[0]
    for (i = 1; i < len; i = i + 1) {
        if (nums[i] > max_val) {
            max_val = nums[i]
        }
    }
    return max_val
}
```

**Functional style:**
```aether
import std.string

// Recurse over the tail, then keep whichever of the head or the
// best-of-the-tail is longer.
longest(words: *StringSeq) {
    match words {
        []      -> { return "" }
        [h | t] -> {
            best_tail = longest(t)
            if (string.length(h) >= string.length(best_tail)) { return h }
            return best_tail
        }
    }
    return ""
}

main() {
    words = string.seq_empty()
    words = string.seq_cons("hi", words)
    words = string.seq_cons("elephant", words)
    words = string.seq_cons("cat", words)
    println(longest(words))  // elephant
    string.seq_free(words)
}
```

### When to Use Each Style

**Use imperative style when:**
- Performance is critical (loops are often faster)
- The algorithm naturally involves mutation
- Working with fixed-size arrays by index

**Use functional style when:**
- Processing linked data structures
- The logic follows recursive definitions (trees, etc.)
- You want self-documenting code

### Erlang Heritage

Aether's pattern matching in receive blocks is inspired by Erlang:

```aether,fragment
actor Calculator {
    receive {
        Add(x, y) -> {
            print("Sum: ")
            print(x + y)
            print("\n")
        }
        Multiply(a, b) -> {
            print("Product: ")
            print(a * b)
            print("\n")
        }
    }
}
```

This is similar to Erlang's pattern matching in receive statements:
```erlang
receive
    {add, X, Y} -> X + Y;
    {multiply, A, B} -> A * B
end.
```

### Example 4: Prime Number Check

```aether
is_prime(n: int) -> bool {
    if (n < 2) {
        return false
    }

    for (i = 2; i < n; i = i + 1) {
        if (n / i * i == n) {
            return false  // divisible, not prime
        }
    }

    return true
}

main() {
    numbers = [2, 3, 4, 5, 6, 7, 8, 9, 10, 11]

    for (i = 0; i < 10; i = i + 1) {
        num = numbers[i]
        if (is_prime(num)) {
            print(num)
            print(" is prime\n")
        }
    }
}
```

## Exercises

### Exercise 1: Absolute Value

Write a function that returns the absolute value of a number:

```aether
abs(n) {
    // Your code here
}

main() {
    print(abs(-5))   // Should print 5
    print("\n")
    print(abs(3))    // Should print 3
    print("\n")
}
```

<details>
<summary>Solution</summary>

```aether,fragment
abs(n) {
    if (n < 0) {
        return -n
    }
    return n
}
```
</details>

### Exercise 2: Power Function

Implement x^n (x to the power of n):

```aether
power(x, n) {
    // Your code here
}

main() {
    print(power(2, 3))   // Should print 8
    print("\n")
    print(power(5, 2))   // Should print 25
    print("\n")
}
```

<details>
<summary>Solution</summary>

```aether,fragment
power(x, n) {
    result = 1
    for (i = 0; i < n; i = i + 1) {
        result = result * x
    }
    return result
}
```
</details>

### Exercise 3: FizzBuzz

Classic programming challenge:
- Print numbers 1 to 20
- For multiples of 3, print "Fizz"
- For multiples of 5, print "Buzz"
- For multiples of both, print "FizzBuzz"

<details>
<summary>Solution</summary>

```aether
main() {
    for (i = 1; i <= 20; i = i + 1) {
        if (i / 15 * 15 == i) {
            print("FizzBuzz\n")
        } else {
            if (i / 3 * 3 == i) {
                print("Fizz\n")
            } else {
                if (i / 5 * 5 == i) {
                    print("Buzz\n")
                } else {
                    print(i)
                    print("\n")
                }
            }
        }
    }
}
```
</details>

## Best Practices

### 1. Keep Functions Small

```aether,fragment
// Good: One clear purpose
calculate_area(width, height) {
    return width * height
}

// Bad: Doing too much
process_everything(a, b, c, d, e) {
    // ... 100 lines of code
}
```

### 2. Use Meaningful Names

```aether,fragment
// Good
calculate_average(numbers) { }
is_valid_email(email) { }

// Bad
calc(n) { }
check(e) { }
```

### 3. Avoid Deep Nesting

```aether,fragment
// Bad: Too nested
if (a) {
    if (b) {
        if (c) {
            if (d) {
                // ...
            }
        }
    }
}

// Good: Early returns
process(a, b, c, d) {
    if (!a) { return 0 }
    if (!b) { return 0 }
    if (!c) { return 0 }
    if (!d) { return 0 }

    // Main logic here
}
```

## Next Steps

You've learned:
- How to write functions
- Type inference for functions
- If/else conditionals
- While and for loops
- Recursive functions

**Next Tutorial:** [Actor Concurrency](03-actor-concurrency.md)

## Quick Reference

```aether,fragment
// Function
add(a, b) {
    return a + b
}

// If statement
if (condition) {
    // code
} else {
    // code
}

// While loop
while (condition) {
    // code
}

// For loop
for (i = 0; i < 10; i = i + 1) {
    // code
}

// Comparisons
==  !=  <  >  <=  >=
```

Happy coding!

