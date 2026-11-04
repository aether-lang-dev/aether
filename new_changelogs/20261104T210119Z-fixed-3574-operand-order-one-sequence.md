- **A long chain of calls compiles with Clang (#3574).** To keep the
  calls in source order, each operator of a chain like
  `f(0) + f(1) + ... + f(n)`, and each call whose arguments are calls,
  held its operands in a C statement expression of its own, inside the one
  of the operator around it: three brackets deeper per term. Clang refuses
  C nested more than 256 deep, so a sum of about 86 calls did not build on
  macOS or with Clang anywhere (GCC has no such limit). One sequence now
  holds the operands of the whole chain, of calls inside calls too, and
  the C nests by a constant whatever the length: a 1000-term sum of calls
  nests 5 deep.
