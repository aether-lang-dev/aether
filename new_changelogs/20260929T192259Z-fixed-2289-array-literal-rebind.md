- **An array literal bound inside a loop body or branch builds (#2289).**
  Such a local is declared once for the whole function, so its binding in
  the body became `kinds = {1, 2, 3}`, a C initializer used as an
  expression, and the build failed with "expected expression before '{'".
  A second binding of an array at function scope failed the same way. The
  literal is now stored element by element, in order, and a shorter literal
  zeroes the rest of the array. A longer one is a compile error naming both
  sizes, since an array keeps the size of its first binding.
