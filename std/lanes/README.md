# std.lanes

SIMD lanes: four single-precision values in one register (`f32x4`), two
double-precision ones (`f64x2`), and the integer vector a lane comparison
yields (`i32x4`), which is the mask `select4` takes.

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

## Exports

`f32x4`, `splat4`, `load4`, `store4`, `lane4`, `sum4`, `min4`, `max4`,
`select4`, `sqrt4`, `abs4`, `lt4`, `le4`, `gt4`, `ge4`, `eq4`; `mask_and`,
`mask_or`, `mask_not`, `any4`, `all4`, `mask_lane`; `f64x2`, `splat2`,
`load2`, `store2`, `lane2`, `sum2`, `min2`, `max2`, `sqrt2`, `abs2`,
`select2`, `lt2`, `le2`, `gt2`, `ge2`, `eq2`, `mask2_and`, `mask2_or`,
`mask2_not`, `any2`, `all2`, `mask2_lane`.

## Requirements

The types lower to the GCC/Clang vector extensions
(`__attribute__((vector_size(16)))`), which every compiler the toolchain
drives has — gcc, clang and `zig cc` on every target. A compiler without them
defines `AETHER_HAS_LANES` as 0 and the generated file simply carries no lane
types; a program that uses one will not compile there, and one that does not
is unaffected. Each entry point here is a `static inline`
helper in the generated translation unit, so a lane operation is the
instruction it names, not a call.
