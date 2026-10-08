- **A string keeps a NUL and every byte after it, from the literal to the
  output (#2469, #2515, #2520, #2521).** A literal such as `"a\x00b"` had
  length 1, plain and interpolated alike (`"a\0b ${n}"` lost the `${n}`
  too); its decoded length now travels from the lexer to codegen, and a
  literal holding a NUL is a static, pinned string carrying its length
  (`string.free` on it is a no-op), also when its std module is read from a
  compiled `.aea` artifact, whose format is now version 2. Maps, sets,
  string lists, `fs.read`, number parsing and `std.json` read a string by
  its length: `"a\0x"` and `"a\0y"` were one map key, a 5-byte file
  `"ab\0cd"` read back as 2 bytes, and `string.to_int` of `"12\0"` +
  `"99"` returned 12 (a number must now end where the string does). `==`
  and `!=` compare length and bytes, the ordering operators compare the
  whole string byte by byte, and so do function-clause guards. A `${s}`
  segment and `print`/`println` of a string value write it by its length
  (`"[${s}]"` with `s` = `"x\0y"` had length 3). A C extern whose parameter
  is `string` still receives the bytes up to the first NUL, all a
  `const char*` can carry.
- **A string is freed whole wherever the allocator put its bytes (#2549).**
  `string_release` takes a string whose bytes start right after its header
  for one allocation, and a string built as two (the header, then the
  bytes) could be laid out just so by an allocator that hands out
  neighbouring blocks of one size: LeakSanitizer's and macOS malloc do. The
  release then freed the header and leaked the bytes, as std.spec's suite
  names did under contrib's leak check. `string_new_with_length` now builds
  one allocation, which is also one call fewer, and a string that adopts
  bytes allocated on their own never gets the header just before them.
- **An escape in an interpolated string means what it means in a plain
  literal (#2512).** `"a\\0b ${n}"` holds a backslash followed by `0b`,
  `"${n}a\\nb"` a backslash and `n`, not a newline; the escapes were decoded
  twice.
- **Each `print` or `println` is one write under stdout's lock (#2521).**
  An interpolation, a string value and its newline went out in separate
  writes, so lines printed by two threads could interleave.
- **A `print` format conversion with no argument is a compile error
  (#2522).** `print`'s literal is a printf format, and a conversion with
  nothing to fill it read the C stack: `print("100% done\n")` printed
  `100 1501462000one` and `print("a %s\n")` crashed. The error names the
  conversion and says to write `%%` for a percent sign, and a `print` whose
  only argument is a literal is written as its decoded text, with no printf
  at run time.
- **Two string-literal `match` arms sharing their first 160 characters are
  not duplicates (#2521).** The reachability check keyed each arm in a
  160-byte buffer, so the second drew a W1004 "already handled" warning,
  which fails builds that treat warnings as errors.
