- **An integer division or remainder by zero in a constant expression is a
  compile error that names the expression (#3524).** `const B = 10 / 0`
  reached the C compiler, which refused the initializer or folded it to
  anything, and `x = 10 / (5 - 5)` compiled to a division that could only
  panic. The compiler computes an expression of literals, `const`s and the
  operators over them, in their own width with the runtime's rules, and
  refuses one whose divisor is 0: "division by zero in the constant
  expression `10 / 0`". A divisor known only at run time still panics with
  `division_by_zero`, and a local or parameter named like a constant is that
  binding.
