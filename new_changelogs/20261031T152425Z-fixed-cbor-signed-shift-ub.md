- **`std.cbor` no longer shifts a set bit into a signed long's sign bit, and
  an empty slice no longer offsets a null pointer.** The new fuzz harness,
  under UBSan, found five places in the decoder that assembled 64-bit values
  with signed left shifts: a 64-bit argument with its top bit set
  (`1b ff…`), a float32 or float16 with its sign bit, a float64 read
  byte by byte, and `diagnose`'s zero test. Each is undefined in C, though
  gcc and clang happen to wrap, so the values were right. They are now
  assembled as `uint64`. `runtime/aether_slice.h` sub-sliced an empty slice
  with a null store as `NULL + 0`, undefined in C though not in C++,
  reached by decoding an empty byte string; offset 0 now returns the pointer
  unchanged.
