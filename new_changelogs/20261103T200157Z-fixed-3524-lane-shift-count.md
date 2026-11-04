- **A lane shift's count is masked to the lane width, as a scalar shift's is
  (#3524).** `lanes.i32shl`, `i32shr`, `i32shr_u`, `i16shl` and `i16shr`
  passed the count to C's vector shift as it was, where a count of the lane
  width or more is undefined (x86 gave 0). The count is now `n & 31` for
  `i32x4` and `n & 15` for `i16x8`, so `lanes.i32shl(a, 32)` is `a`, and a
  left shift into the sign bit wraps. A constant count inside the width is
  still the plain shift instruction.
