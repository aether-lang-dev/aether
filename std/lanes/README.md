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
`i32x4`, 8 for `f64x2`): an element index, like an `f32[]` view's, **not**
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
it does not have: the compiler says so, at the source line.

A comparison (`<`, `<=`, `>`, `>=`, `==`, `!=`, or the `lt4`/`le4`/… spellings)
yields an integer vector of the same width whose lane is all-ones where it
held: `i32x4` for `f32x4`, `i64x2` for `f64x2`. Feed it to `select4` /
`select2`, combine masks with `mask_and` / `mask_or` / `mask_not` (or
`mask2_and` / `mask2_or` / `mask2_not`), or reduce it to a `bool` with
`any4` / `all4` / `any2` / `all2` when a branch is wanted. A scalar operand
splats in a comparison as it does in arithmetic: `v > 2.0` compares every
lane against 2. A scalar variable splats as a literal does (`v * s` with a
`float` `s` is converted to the lane's element type first), on either side
of the operator.

`sqrt4` / `sqrt2` and `abs4` / `abs2` are the two libm shapes that belong
here. Both are EXACT (an IEEE square root is correctly rounded, and
clearing the sign bit is not an approximation), so the lane form gives the
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

**On Windows, builds assemble every vector move unaligned.** The Win64
stack is only 16-byte aligned and GCC does not realign it for 256-bit values
(GCC bug 54412), yet it can spill them with the aligned `vmovaps` / `vmovdqa`,
which fault when the slot is not 32-byte aligned (#2476: an `f32x8` solver
built with `-mavx2` crashed under MinGW GCC 15). `ae build` and `ae run` pass
`-Wa,-muse-unaligned-vector-move` (binutils 2.38 or later) to every Windows
build, since an `@target("avx2")` import is AVX code whatever the cflags,
and `ae cflags --cflags` carries it for a build that calls gcc itself
(#3023). The assembler encodes those moves as `vmovups` / `vmovdqu`: the
same speed on aligned data, no fault on the rest. An older assembler is
used without it, with a warning when the cflags enable AVX. Other platforms
are unchanged.

**On Windows, GCC at `-O0` uses the two halves.** An `f32x8` is declared
16-byte aligned (heap storage guarantees no more), and GCC on Win64 stops
with an internal compiler error (`expand_one_stack_var_1`) on such a local
when it is not optimizing. So without optimization there, `-mavx2` gives
the two-halves form and an `@target("avx2")` copy is plain code (#3023): a
debug build, so the width does not matter. From `-O1` up, and with Clang,
nothing changes.

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

For the integer image and audio kernels (a JPEG IDCT, YCbCr→RGB, PNG
filters, PCM mixing) `i32x4` (four 32-bit lanes) and `i16x8` (eight 16-bit
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

A shift count is masked to the lane width, as a scalar shift's is (#3524):
`n & 31` for `i32x4`, `n & 15` for `i16x8`, so `lanes.i32shl(a, 33)` shifts by
1. A left shift into the sign bit wraps. A constant count inside the width is
the plain shift instruction.

The hot exit of these kernels is a **saturating pack**: narrow wide
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
check that the WHOLE vector width fits before decaying it to a pointer
(`load4_slice`/`store4_slice` and `load8_slice`/`store8_slice` (`f32[]`), `load2_slice`/`store2_slice`
(`float[]`), `i32load_slice`/`i32store_slice` (`int[]`),
`i16load_slice`/`i16store_slice` (`uint16[]`) the lane width the array
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
Aether slice at all: an external C buffer, or one reached through `std.mem`
without going via `make`/a fixed array.

## Moves between lanes (#2550)

A structure-of-arrays kernel gathers records into lanes: eight bodies'
velocities into one `f32x8` per component. Eight row loads and an 8x8
transpose in registers do it, where one `f32x8(...)` per field costs eight
scalar loads and eight inserts. The moves are the patterns a transpose is
made of, each one instruction (unpcklps, movlhps, shufps, vperm2f128 on x86;
zip and ext on NEON):

| move | lanes |
|---|---|
| `unpacklo4(a, b)` / `unpackhi4(a, b)` | `[a0 b0 a1 b1]` / `[a2 b2 a3 b3]` |
| `pairs_lo4(a, b)` / `pairs_hi4(a, b)` | `[a0 a1 b0 b1]` / `[a2 a3 b2 b3]` |
| `unpacklo8` / `unpackhi8` / `pairs_lo8` / `pairs_hi8` | the four-lane move on lanes 0-3 and on lanes 4-7 of each, as AVX's own work within each 128-bit half |
| `halves_lo8(a, b)` / `halves_hi8(a, b)` | `[a0 a1 a2 a3 b0 b1 b2 b3]` / `[a4 a5 a6 a7 b4 b5 b6 b7]` |

```aether,run
import std.lanes

main() {
    a = lanes.f32x8(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0)
    b = a + 8.0
    lo = lanes.unpacklo8(a, b)      // 1 9 2 10 5 13 6 14
    hi = lanes.halves_hi8(a, b)     // 5 6 7 8 13 14 15 16
    println("${lanes.lane8(lo, 1)} ${lanes.lane8(lo, 4)} ${lanes.lane8(hi, 0)} ${lanes.lane8(hi, 7)}")
}
```
```output
9 5 5 16
```

An 8x8 transpose of rows `r0` to `r7`, as Box3D's `b3TransposeW8` does it:
unpack row pairs (`t0 = unpacklo8(r0, r1)`, `t1 = unpackhi8(r0, r1)`, and so
on to `t7`), take pairs of those (`u0 = pairs_lo8(t0, t2)`, `u1 =
pairs_hi8(t0, t2)`, `u2 = pairs_lo8(t1, t3)`, `u3 = pairs_hi8(t1, t3)`, the
same from `t4`-`t7` into `u4`-`u7`), then halves: column k is
`halves_lo8(u(k), u(k + 4))` for k below 4 and `halves_hi8(u(k - 4), u(k))`
from 4 on. The four-lane transpose is `unpacklo4` / `unpackhi4` of rows 0/1
and 2/3, then `pairs_lo4` / `pairs_hi4` of those. Under `-mavx2` the
eight-lane one is 24 shuffles on ymm registers; without it, each half is
the four-lane one.

## Bits, eight integer lanes, reductions (#2603)

`bits4` / `bits8` read a float lane's bits as an integer lane, and
`from_bits4` / `from_bits8` go back: a reinterpretation, not a conversion.
With `mask_and` / `mask_or` (`mask8_and` / `mask8_or`), which are bitwise on
any integer lanes, a float's bits are edited in place. `i32x8(a, ..., h)` and
`i32splat8(v)` build eight integer lanes, whose arithmetic operators and
comparisons work as on `f32x8`.

`reduce_min4` / `reduce_max4` / `reduce_min8` / `reduce_max8` give the
smallest or largest lane. The lanes are compared in one fixed order, with
`min4`'s rule `a < b ? a : b`: for eight, lane i against lane i + 4 first,
then lanes 0 and 2, 1 and 3, then the two winners. A tie or a NaN gives the
same answer on every instruction set and in either form of `f32x8`.

```aether,run
import std.lanes

main() {
    bits = lanes.bits8(lanes.splat8(1.0))
    v = lanes.f32x8(3.0, 1.5, 9.0, 0.25, 7.0, 0.25, 2.0, 5.0)
    println("${lanes.mask8_lane(bits, 0)} ${lanes.reduce_min8(v)} ${lanes.reduce_max8(v)}")
}
```
```output
1065353216 0.25 9
```

Together they keep an index with a value, as Box3D's support search does
(`b3EmbedIndexW8` / `b3MinIndexW8`): the low mantissa bits of each positive
candidate carry its index, and one `reduce_min8` gives the smallest value
and, in its low bits, where it was. Ties go to the lower index.

```aether,fragment
index = lanes.i32x8(0, 1, 2, 3, 4, 5, 6, 7) + lanes.i32splat8(base)
tagged = lanes.from_bits8(lanes.mask8_or(lanes.mask8_and(lanes.bits8(d), lanes.i32splat8(0 - 128)), index))
best = lanes.mask8_lane(lanes.bits8(lanes.splat8(lanes.reduce_min8(tagged))), 0) % 128
```

## One binary, two widths (#2607)

A build picks one width for `f32x8`: one register under `-mavx2`, which
faults on a CPU without AVX2, or two halves everywhere. A program that ships
one executable imports the module holding its lane code a second time, at a
target:

```aether,fragment
import std.lanes
import physics.wide
@target("avx2") import physics.wide as wide8

solve(constraints: ptr, n: int) {
    if lanes.cpu_has_avx2() {
        wide8.solve(constraints, n)    // eight lanes in one ymm register
    } else {
        wide.solve(constraints, n)     // the build's own width
    }
}
```

The `@target("avx2")` import is a copy of the module of its own, compiled
for AVX2 while the rest of the program keeps the build's flags, and called
through its `as` name. Both copies compute the same bits. `lanes.native8()`
says which form the code calling it has (true in the copy), and
`lanes.cpu_has_avx2()` whether this CPU runs it (false off x86).

The two copies hold `f32x8` as different C types, so an eight-lane value
stays on its side: a call between the copies (or into C compiled apart)
that passes an `f32x8`, an `i32x8`, or a struct or pointer holding one, is a
compile error. Pass scalars and pointers to the data across, as Box3D's
`wide4.c` / `wide8.c` entry points do. A closure inside the copy is also an
error: its body would be compiled outside the copy. With a compiler other
than GCC or Clang on x86, on the `AETHER_GCC_COMPAT=0` path, and with GCC on
Windows at `-O0` (#3023), the copy is plain code with no target attribute:
it still runs and agrees, and `native8()` is false there.

## Requirements

The types lower to the GCC/Clang vector extensions
(`__attribute__((vector_size(16)))`, and `(32)` for an AVX2 `f32x8`), which
every compiler the toolchain drives has: gcc, clang and `zig cc` on every target. A compiler without them
defines `AETHER_HAS_LANES` as 0 and the generated file simply carries no lane
types; a program that uses one stops there at one `#error` that says so
(MSVC's cl.exe is such a compiler), and one that does not is unaffected.
Each entry point here is a `static inline`
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

Moves: `unpacklo4`, `unpackhi4`, `pairs_lo4`, `pairs_hi4`, `unpacklo8`,
`unpackhi8`, `pairs_lo8`, `pairs_hi8`, `halves_lo8`, `halves_hi8`.

Bits and reductions: `bits4`, `from_bits4`, `bits8`, `from_bits8`, `i32x8`,
`i32splat8`, `reduce_min4`, `reduce_max4`, `reduce_min8`, `reduce_max8`.

Widths: `native8`, `cpu_has_avx2`.

Integer: `i32x4`, `i32splat`, `i32load`, `i32store`, `i32lane`, `i32sum`,
`i32add`, `i32sub`, `i32mul`, `i32shl`, `i32shr`, `i32shr_u`, `i32min`,
`i32max`; `i16x8`, `i16splat`, `i16load`, `i16store`, `i16lane`, `i16add`,
`i16sub`, `i16mul`, `i16shl`, `i16shr`, `i16min`, `i16max`; `pack_i32_i16`,
`pack_i16_u8`.

Slice-checked: `load4_slice`, `store4_slice`, `load2_slice`, `store2_slice`,
`load8_slice`, `store8_slice`, `i32load_slice`, `i32store_slice`,
`i16load_slice`, `i16store_slice`, `pack_i16_u8_slice`.
