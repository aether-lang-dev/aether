- **An escape in an interpolated string means what it means in a plain
  literal (#2512).** The text of `"...${x}..."` takes the same escapes, with
  the same meaning, wherever they sit: `"a\\0b ${n}"` holds a backslash
  followed by `0b`, `"\\u0000 ${n}"` keeps its backslash, `"${n}a\\nb"` is a
  backslash and `n`, not a newline, and `\${` writes a literal `${`.
- **String comparisons read the whole string (#2515).** `==` and `!=` on
  strings compare length and bytes, so a string holding `"x\0y"` is not equal
  to `"x"`, and `<`, `<=`, `>` and `>=` order strings byte by byte over their
  whole length. Two optional strings compare their values the same way. A
  string comparison in a function-clause guard, `f(s) when s == "bob"`,
  compares the strings too.
