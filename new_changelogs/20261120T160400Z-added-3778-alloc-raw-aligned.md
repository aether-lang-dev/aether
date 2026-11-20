- **`alloc.raw_aligned(a, size, alignment)` and `alloc.release_aligned`.** A
  block at an address that is a multiple of `alignment`, a power of two at
  least the size of a pointer: 64 lets a record own a cache line, which
  malloc's 16 bytes do not promise. Through any allocator: the system one
  uses the platform's aligned allocator, and any other (an arena, a tracking
  wrapper, a host's) is asked for the alignment more. Asked by aephysics,
  whose solver keeps each body's state in one 64-byte line (#3778).
