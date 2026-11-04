- **Concatenated, sliced, cased, trimmed, replaced and joined strings are
  one allocation instead of two (#3530).** Each wrote its text into a
  buffer of its own and then allocated a separate header for it: two
  mallocs, two frees, and the header and text on different cache lines.
  Their length is known before they are written, so they are now built in
  the one block, header then text, that string literals and interpolations
  already used, and `string.concat_wrapped` no longer copies its result a
  second time. Best of 5 on this machine: `string.concat` 61.8 to 34.3 ns,
  `string.substring` 59.4 to 31.8 ns, the ownership `concat` program 205
  to 125 ms.
