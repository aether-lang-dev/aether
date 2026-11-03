- **An integer division or remainder by zero in a constant's initializer is
  a compile error that names the expression (#3524).** `const B = 10 / 0`
  reached the C compiler, which refused the static's initializer or folded
  it to anything. The compiler computes the initializer of a `const` or a
  module `var` (literals, `const`s and the operators over them, in their own
  width with the runtime's rules) and refuses one that divides by 0:
  "division by zero in the constant expression `10 / D`", with each name as
  it was written, a module's constant included. The same division in code,
  guarded or never run (`if D != 0 { println(100 / D) }`), compiles as in
  Java, and panics with `division_by_zero` if it runs.
