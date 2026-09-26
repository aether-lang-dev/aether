- **`std.lanes` gains integer lanes: `i32x4` (four 32-bit) and `i16x8` (eight
  16-bit) (#2212).** std.lanes had float lanes only; the integer image and
  audio kernels stb-image vectorises — a JPEG IDCT, YCbCr->RGB, PNG filters,
  PCM mixing — ran scalar. Now: load / store / splat / set / lane-read for
  both widths; add / sub / mul; shifts by a constant count (`i32shl`,
  arithmetic `i32shr`, logical `i32shr_u`, and the i16 pair); lane-wise
  `min` / `max`; and the two saturating packs the kernels exit through —
  `pack_i32_i16` (i32x4 -> i16x8, signed) and `pack_i16_u8` (i16x8 -> sixteen
  u8, unsigned). Like the float lanes these lower to the GCC/Clang vector
  extensions (SSE2 / NEON at -O2); the packs take `packssdw` / `packuswb`
  where present and a clamped scalar loop otherwise, always saturating rather
  than wrapping. `tests/integration/lane_integer` covers every op at its
  boundaries plus an IDCT-shaped kernel checked against a scalar reference.
  This lets ae3d.jpeg close its remaining SIMD gap without C.
