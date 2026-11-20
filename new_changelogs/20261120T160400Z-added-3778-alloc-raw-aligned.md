- **`alloc.raw_aligned(a, size, alignment)` and `alloc.release_aligned`.** A
  block at an address that is a multiple of `alignment`, a power of two at
  least the size of a pointer: 64 lets a record own a cache line, which
  malloc's 16 bytes do not promise. Through any allocator, the same way: it
  is asked for `alignment - 1` bytes and a pointer's more, and the block is
  the first aligned address in that. Asked by aephysics, whose solver keeps
  each body's state in one 64-byte line (#3778).
