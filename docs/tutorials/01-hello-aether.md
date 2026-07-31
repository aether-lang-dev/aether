# Tutorial 1: Hello, Aether!

**Time:** 30 minutes  
**Prerequisites:** Basic programming knowledge  
**Goal:** Write and run your first Aether program

## Introduction

Welcome to Aether! In this tutorial, you'll learn the basics of the Aether programming language and write your first program.

## What is Aether?

Aether is a programming language that combines:
- **Erlang's concurrency model** - Lightweight actors with message passing
- **Type inference** - Automatic type deduction for local variables  
- **C's performance** - Compiles to optimized C code with zero runtime overhead

## Installation

If you haven't installed Aether yet, follow the installation guide:

```bash
git clone https://github.com/aether-lang-dev/aether.git
cd aether

# Build the ae CLI tool (includes compiler)
make ae
```

Verify installation:
```bash
./build/ae version
```

## Your First Program

### Step 1: Create a File

Create a file called `hello.ae`:

```aether
main() {
    print("Hello, Aether!\n")
}
```

**What's happening here?**
- `main()` is the entry point of every Aether program
- `print()` outputs text to the console
- `\n` adds a newline

### Step 2: Run It

```bash
./build/ae run hello.ae
```

You should see:
```
Hello, Aether!
```

**Behind the scenes:**
1. Aether compiles `hello.ae` to C code
2. GCC compiles the C code to an executable
3. The program runs immediately

## Variables and Type Inference

Aether automatically figures out types - you don't need to declare them!

```aether
main() {
    // Numbers
    x = 42            // inferred as int
    pi = 3.14         // inferred as float
    
    // Text
    name = "Alice"    // inferred as string
    
    // Print them
    print("x = ")
    print(x)
    print("\n")
    
    print("pi = ")
    print(pi)
    print("\n")
    
    print("name = ")
    print(name)
    print("\n")
}
```

**Try it:**
```bash
./build/ae run variables.ae
```

## Basic Math

```aether
main() {
    a = 10
    b = 5
    
    sum = a + b
    diff = a - b
    product = a * b
    quotient = a / b
    
    print("Sum: ")
    print(sum)
    print("\n")
    
    print("Product: ")
    print(product)
    print("\n")
}
```

## Comments

```aether
main() {
    // This is a single-line comment
    print("Hello!\n")  // Comment after code
    
    /*
     * This is a multi-line comment
     * Useful for longer explanations
     */
}
```

## Exercises

### Exercise 1: Personal Greeting

Create a program that prints your name and age:

```aether
main() {
    name = "YourName"
    age = 25
    
    // Your code here
    // Print: "My name is [name] and I am [age] years old."
}
```

<details>
<summary>Solution</summary>

```aether
main() {
    name = "Alice"
    age = 25
    
    print("My name is ")
    print(name)
    print(" and I am ")
    print(age)
    print(" years old.\n")
}
```
</details>

### Exercise 2: Temperature Converter

Convert Celsius to Fahrenheit using the formula: F = (C × 9/5) + 32

```aether
main() {
    celsius = 25
    
    // Calculate fahrenheit
    fahrenheit = (celsius * 9 / 5) + 32
    
    // Print result
    print(celsius)
    print(" Celsius = ")
    print(fahrenheit)
    print(" Fahrenheit\n")
}
```

### Exercise 3: Circle Area

Calculate the area of a circle with radius 5.
Formula: Area = π × r²

```aether
main() {
    pi = 3.14159
    radius = 5
    
    // Calculate area
    area = pi * radius * radius
    
    print("Circle area: ")
    print(area)
    print("\n")
}
```

## Common Mistakes

### Mistake 1: Forgetting Parentheses

```aether
// Wrong
main {
    print("Hello")
}

// Correct
main() {
    print("Hello")
}
```

### Mistake 2: Missing Newlines

```aether
// Prints on same line
print("Hello")
print("World")
// Output: HelloWorld

// Add \n for newlines
print("Hello\n")
print("World\n")
// Output:
// Hello
// World
```

## Next Steps

You've learned:
- How to run Aether programs
- Variables and type inference
- Basic math operations
- Comments

**Next Tutorial:** [Functions and Control Flow](02-functions-and-control-flow.md)

## Quick Reference

```aether
// Variables (type inferred)
x = 42
name = "Alice"
pi = 3.14

// Math
sum = a + b
diff = a - b
product = a * b
quotient = a / b

// Output
print("Text\n")
print(variable)

// Comments
// Single line
/* Multi
   line */

// Entry point
main() {
    // Your code here
}
```

## Help

- **Compiler errors?** Check for typos and missing parentheses
- **Program won't run?** Verify GCC is installed: `gcc --version`
- **Questions?** Open an issue on GitHub

Happy coding!

