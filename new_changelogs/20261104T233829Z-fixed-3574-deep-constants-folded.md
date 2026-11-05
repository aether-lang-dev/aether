- **Long chains in constant initializers compile with Clang (#3574).** A
  file-scope `const`'s initializer, and a module variable's, is a C
  constant expression, which can hold no temporary, so a long chain in
  one still nested its C a level a link: 1000 comparisons were 1000
  brackets, 1000 shifts 3999, past the 256 Clang takes. Past 32 levels
  the compiler now computes such an initializer, as the C would, and
  writes the value.
