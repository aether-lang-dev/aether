- **Comparing a string with the literal `"0"` compares by content again
  (#2206).** `s != "0"` was emitted as a bare C pointer compare against
  the `.rodata` literal (never equal, and clang's `-Wstring-compare`
  warned about it), while `s != "1"` or `s != "abc"` went through
  `strcmp` as intended. The string-compare codegen recognised the
  integer literal `0` as a null check by its spelling alone, and the
  one-character string literal `"0"` shares that spelling. The check now
  ignores string-typed literals; `p == 0` and `p == NULL` still lower
  to pointer null checks.
