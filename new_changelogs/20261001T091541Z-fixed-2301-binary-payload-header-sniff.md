- **A binary write no longer misreads a payload that begins with the
  string-header magic.** `fs.write_binary` / `fs.write_atomic` checked their
  data in C for an AetherString header after the generated call had already
  unwrapped it, and a raw pointer (`bytes.data(buf)`) passed to their
  `string` parameter went through the same check on the way in. A payload
  starting `DE C0 57 AE` was therefore taken for a header, and the write
  followed a `data` pointer read out of the payload's own bytes, writing
  whatever memory that named or crashing. Binary data now reaches C as a raw
  `ptr` from a `byte[]` slice and is never checked;
  `tests/regression/test_issue2301_binary_io_slices.ae` writes such a payload
  through every changed path and compares it byte for byte.
