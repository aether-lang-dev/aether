# std.lanes

SIMD lanes: four single-precision values in one register (`f32x4`), two
double-precision ones (`f64x2`), four 32-bit integers (`i32x4`, which also
doubles as the mask `select4` takes), eight 16-bit integers (`i16x8`), and
eight single-precision values (`f32x8`) with their eight-lane mask
(`i32x8`).

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

## Eight float lanes (#2428)

`f32x8` is the `f32x4` surface at twice the width: `f32x8(a, ..., h)`,
`splat8`, `load8` / `store8` (32 bytes at an element index), `lane8`, `sum8`,
`min8` / `max8`, `select8`, `sqrt8`, `abs8`, the comparisons `lt8` / `le8` /
`gt8` / `ge8` / `eq8`, and an `i32x8` mask with `mask8_and` / `mask8_or` /
`mask8_not`, `any8` / `all8` and `mask8_lane`. Operators work as they do on
`f32x4`. `.x` to `.w` read lanes 0-3; `lane8` reads any of the eight.

```aether,run
import std.lanes

main() {
    v = lanes.f32x8(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0)
    w = v * 2.0 - 1.0                          // eight lanes, two operators
    upper = lanes.select8(w > 8.0, w, lanes.splat8(0.0))
    println("${lanes.lane8(w, 7)} ${lanes.sum8(w)} ${lanes.sum8(upper)}")
}
```
```output
15 64 48
```

**The width is a build choice, not a source one.** With AVX2 enabled, an
`f32x8` is one 256-bit register and each operation one instruction. Turn it
on in aether.toml:

```toml
[build]
cflags = "-mavx2"        # or "-march=native" for the machine that builds it
```

Without AVX2 (the default x86-64 build, and ARM's NEON), an `f32x8` is two
four-lane halves held in two registers, so it costs exactly what two `f32x4`
operations cost. The program and its results are the same either way: a
kernel written eight-wide is never slower than its four-lane version, and is
twice as wide where AVX2 is on. Sums add lanes 0..7 in order, as a scalar
loop would, so `sum8` is the same bits in either form.

Measured on an eight-term polynomial with a clamp, per element, over 64 Ki
floats (`-O2`, Windows/MinGW, i7-13700K):

| build | `f32x4` | `f32x8` |
|---|---|---|
| default | 99 ms | 97-105 ms |
| `-mavx2` | 98 ms | **50 ms** |

A kernel bound by the square root gains less: this CPU's 256-bit `vsqrtps`
retires at half the rate of the 128-bit one, so `sqrt8` under AVX2 matches
two `sqrt4`s.

The two forms are different C types, so C compiled separately that takes or
returns an `f32x8` by value has to use the same `-m` flags as the program.

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

## Slice-checked wrappers (#2301)

Every load/store above takes a raw `ptr` and trusts the caller's bound, like
`std.mem`'s accessors. The `_slice` siblings take a typed slice instead and
check that the WHOLE vector width fits before decaying it to a pointer —
`load4_slice`/`store4_slice` and `load8_slice`/`store8_slice` (`f32[]`), `load2_slice`/`store2_slice`
(`float[]`), `i32load_slice`/`i32store_slice` (`int[]`),
`i16load_slice`/`i16store_slice` (`uint16[]` — the lane width the array
element type matches; there is no first-class signed 16-bit array element),
and `pack_i16_u8_slice` (`out: byte[]`, needing room for all 16 bytes). An
out-of-range call panics naming the lane count, the element index and the
slice's length, rather than reading or writing past the buffer:

```aether,fragment
buf = make([]f32, 4)
v = lanes.load4_slice(buf, 0) * lanes.splat4(0.5)
lanes.store4_slice(buf, 0, v)
lanes.load4_slice(buf, 1)   // panics: 4 lanes at element 1 need 5 elements, buf has 4
```

The raw `ptr` forms remain the right choice for a buffer that isn't an
Aether slice at all — an external C buffer, or one reached through `std.mem`
without going via `make`/a fixed array.

## Requirements

The types lower to the GCC/Clang vector extensions
(`__attribute__((vector_size(16)))`, and `(32)` for an AVX2 `f32x8`), which
every compiler the toolchain drives has — gcc, clang and `zig cc` on every target. A compiler without them
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
`mask2_not`, `any2`, `all2`, `mask2_lane`; `f32x8`, `splat8`, `load8`,
`store8`, `lane8`, `sum8`, `min8`, `max8`, `select8`, `sqrt8`, `abs8`, `lt8`,
`le8`, `gt8`, `ge8`, `eq8`, `mask8_and`, `mask8_or`, `mask8_not`, `any8`,
`all8`, `mask8_lane`.

Integer: `i32x4`, `i32splat`, `i32load`, `i32store`, `i32lane`, `i32sum`,
`i32add`, `i32sub`, `i32mul`, `i32shl`, `i32shr`, `i32shr_u`, `i32min`,
`i32max`; `i16x8`, `i16splat`, `i16load`, `i16store`, `i16lane`, `i16add`,
`i16sub`, `i16mul`, `i16shl`, `i16shr`, `i16min`, `i16max`; `pack_i32_i16`,
`pack_i16_u8`.

Slice-checked: `load4_slice`, `store4_slice`, `load2_slice`, `store2_slice`,
`load8_slice`, `store8_slice`, `i32load_slice`, `i32store_slice`,
`i16load_slice`, `i16store_slice`, `pack_i16_u8_slice`.
