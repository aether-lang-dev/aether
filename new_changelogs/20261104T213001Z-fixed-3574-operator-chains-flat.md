- **Long chains of `&&`, `||`, comparisons, `/`, `%`, `<<` and `>>`
  compile with Clang (#3574).** Each link of such a chain wrapped the one
  before it in C of its own: brackets for `&&`, `||` and comparisons, the
  checked division's call for `/` and `%`, the masked shift's casts for
  `<<` and `>>`, one to four brackets a link. Clang refuses C nested more
  than 256 deep, so a chain of about 128 links (64 shifts) did not build.
  `&&` and `||` chains are now written as flat as `+` chains are; the
  others, past 16 links, link by link in one sequence, left to right, with
  a division by zero still raised at its own link, before anything after
  it is evaluated.
