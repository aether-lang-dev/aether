- **The second identical `ae build` hits the cache, and one program leaves
  one entry (#2500).** The first build of a source has no depfile yet, so its
  cache key falls back to a walk of the source tree; aetherc writes the
  depfile during that build, and every later build keys on it. The first
  build published its binary under the tree-walk key, which no later build
  computes, so the second build compiled again and published a second copy,
  and only the third hit. `ae build` now recomputes the key once the depfile
  is written and publishes under that, as `ae run` already did. `ae run` now
  recomputes with the same salt it looked up with, so a program with a
  binary import no longer publishes under a key its next run never asks for.
- **On Windows, an AVX build no longer faults on a 256-bit spill (#2476).**
  The Win64 stack is only 16-byte aligned and GCC does not realign it for
  32-byte values (GCC bug 54412), but it can still spill them with the
  aligned `vmovaps` / `vmovdqa`, so an `f32x8` program built with `-mavx2`
  segfaulted under MinGW GCC 15. When the cflags enable AVX, `ae build` and
  `ae run` on Windows now pass `-Wa,-muse-unaligned-vector-move`, and the
  assembler (binutils 2.38 or later) encodes those moves as `vmovups` /
  `vmovdqu`, which cost the same on aligned data and do not fault on the
  rest. An older assembler gets a warning. Builds without AVX, and every
  other platform, are unchanged.
