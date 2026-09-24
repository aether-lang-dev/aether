- **`vulkan.pixel()` returns a `long`, so opaque white is no longer the error
  value.** It packed `0xRRGGBBAA` into an `int`, so opaque white read as `-1`,
  the same value it returned for a coordinate outside the image, and a caller
  could not tell them apart. Every colour is now non-negative, and `-1` means
  only "no pixel there". `red` / `green` / `blue` / `alpha` take the `long`.
  Existing code keeps compiling unchanged, because a `long` passed where an
  `int` is expected keeps its low 32 bits, which is what the channels read.
  `contrib.d3d12` and `contrib.metal` return the same.

  `contrib.vulkan` also declares its C file with `@source` now, so
  `import contrib.vulkan` is all a build needs; an `extra_sources` entry for
  it still works.
