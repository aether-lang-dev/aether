# std.lanes

SIMD lanes: four single-precision values in one register (`f32x4`), two
double-precision ones (`f64x2`), four 32-bit integers (`i32x4`, which also
doubles as the mask `select4` takes) and eight 16-bit integers (`i16x8`).

The lane types are language types, so arithmetic and comparison are written
as ordinary operators and happen lane-wise; this module is what a lane value
is built from and read back through.

```aether,run
import std.lanes

main() {
    a = lanes.f32x4(1.0, 2.0, 3.0, 4.0)
    b = lanes.splat4(2.0)
    c = a * b + lanes.splat4(1.0)          // one multiply, one add, four lanes
    over = c > lanes.splat4(5.0)           // a mask, not a bool
    kept = lanes.select4(over, c, lanes.splat4(0.0))
    println("${c.x} ${c.w} ${lanes.sum4(c)} ${lanes.sum4(kept)}")
}
```
```output
3 9 24 16
```

`load4(buf, index)` and `store4(buf, index, v)` read and write 16 bytes at
`index` **elements** into a raw buffer (4 bytes per element for `f32x4` and
`i32x4`, 8 for `f64x2`) — an element index, like an `f32[]` view's, **not**
the byte offset `std.mem`'s accessors take. A loop over a float buffer
advances by 4 per `f32x4`, and by 2 per `f64x2`. No alignment is required, and the bounds are the
caller's to know, as they are for `std.mem`'s accessors:

```aether,fragment
raw = malloc(n * 4)
buf = raw as f32[]
i = 0
while i < n {
    v = lanes.load4(raw, i) * lanes.splat4(0.5)
    lanes.store4(raw, i, v)
    i = i + 4
}
```

On a clamp-and-accumulate kernel over 1 Mi floats, the lane version runs
**1.9x** the scalar one with identical results (`-O2`, Windows/UCRT64,
i7-1265U).

**Lane types are nominal.** An `f32x4` is not an `f32`, an `f64x2` or an
array: a scalar becomes lanes through `splat4` / `f32x4`, and lanes become
scalars through `.x` / `.y` / `.z` / `.w`, `lane4` or `sum4`. `a + d` for an
`f32x4` and an `f64x2` is a type error, and `.z` on an `f64x2` names a lane
it does not have — the compiler says so, at the source line.

A comparison (`<`, `<=`, `>`, `>=`, `==`, `!=`, or the `lt4`/`le4`/… spellings)
yields an integer vector of the same width whose lane is all-ones where it
held — `i32x4` for `f32x4`, `i64x2` for `f64x2`. Feed it to `select4` /
`select2`, combine masks with `mask_and` / `mask_or` / `mask_not` (or
`mask2_and` / `mask2_or` / `mask2_not`), or reduce it to a `bool` with
`any4` / `all4` / `any2` / `all2` when a branch is wanted. A scalar operand
splats in a comparison as it does in arithmetic: `v > 2.0` compares every
lane against 2.

`sqrt4` / `sqrt2` and `abs4` / `abs2` are the two libm shapes that belong
here. Both are EXACT — an IEEE square root is correctly rounded, and
clearing the sign bit is not an approximation — so the lane form gives the
same lanes as calling the scalar function four times, in the one instruction
the hardware has had since SSE2 rather than four extracts, four calls and a
rebuild. The transcendentals are deliberately absent: `sin` and `exp` per
lane are approximations with an accuracy choice to make, and a caller who
wants one should make that choice explicitly.

## Integer lanes (#2212)

For the integer image and audio kernels — a JPEG IDCT, YCbCr→RGB, PNG
filters, PCM mixing — `i32x4` (four 32-bit lanes) and `i16x8` (eight 16-bit
lanes) carry the arithmetic those need:

```aether,fragment
import std.lanes

a = lanes.i32x4(1, 2, 3, 4)
b = lanes.i32add(a, lanes.i32splat(10))   // 11,12,13,14; also i32sub / i32mul
lanes.i32shl(a, 2)                        // shift every lane left by 2
lanes.i32shr(a, 1)                        // ARITHMETIC right (sign-preserving)
lanes.i32shr_u(a, 1)                      // LOGICAL right (zero-fill)
lanes.i32min(a, b) / lanes.i32max(a, b)   // lane-wise
lanes.i32load(buf, i) / lanes.i32store(buf, i, a)   // 4 ints at element i
lanes.i32lane(a, 0) / lanes.i32sum(a)     // read one lane / sum (as long)
```

`i16x8` mirrors it (`i16add`, `i16mul`, `i16shl`, `i16shr`, `i16min`,
`i16max`, `i16load`, `i16store`, `i16lane`, `i16splat`, `i16x8`), with 16-bit
lanes read back sign-extended to `int`.

The hot exit of these kernels is a **saturating pack** — narrow wide
accumulators to the next size down, clamping (not wrapping) out-of-range
lanes:

```aether,fragment
// two i32x4 -> one i16x8, SIGNED-saturated (a in lanes 0-3, b in 4-7):
lanes.pack_i32_i16(a, b)                  // 40000 -> 32767, -40000 -> -32768
// two i16x8 -> sixteen u8 written to `out`, UNSIGNED-saturated:
lanes.pack_i16_u8(x, y, out)              // 300 -> 255, -5 -> 0; out holds 16 bytes
```

The packs take the hardware instruction (`packssdw` / `packuswb` on SSE2) where
there is one and a clamped scalar loop otherwise; either way the result is the
saturated value, never a wrapped one.

## Requirements

The types lower to the GCC/Clang vector extensions
(`__attribute__((vector_size(16)))`), which every compiler the toolchain
drives has — gcc, clang and `zig cc` on every target. A compiler without them
defines `AETHER_HAS_LANES` as 0 and the generated file simply carries no lane
types; a program that uses one will not compile there, and one that does not
is unaffected. Each entry point here is a `static inline`
helper in the generated translation unit, so a lane operation is the
instruction it names, not a call.

## Exports

Float: `f32x4`, `splat4`, `load4`, `store4`, `lane4`, `sum4`, `min4`, `max4`,
`select4`, `sqrt4`, `abs4`, `lt4`, `le4`, `gt4`, `ge4`, `eq4`; `mask_and`,
`mask_or`, `mask_not`, `any4`, `all4`, `mask_lane`; `f64x2`, `splat2`,
`load2`, `store2`, `lane2`, `sum2`, `min2`, `max2`, `sqrt2`, `abs2`,
`select2`, `lt2`, `le2`, `gt2`, `ge2`, `eq2`, `mask2_and`, `mask2_or`,
`mask2_not`, `any2`, `all2`, `mask2_lane`.

Integer: `i32x4`, `i32splat`, `i32load`, `i32store`, `i32lane`, `i32sum`,
`i32add`, `i32sub`, `i32mul`, `i32shl`, `i32shr`, `i32shr_u`, `i32min`,
`i32max`; `i16x8`, `i16splat`, `i16load`, `i16store`, `i16lane`, `i16add`,
`i16sub`, `i16mul`, `i16shl`, `i16shr`, `i16min`, `i16max`; `pack_i32_i16`,
`pack_i16_u8`.
