# Codegen-inline the `std.mem` scalar accessors (and give `mem` an offset copy)

**From:** the ebiten-in-aether port (2026-08-24) · **Where it bit:** the
software 2D compositor at the heart of a game engine — per-pixel RGBA work
over a 640×480 framebuffer at 60Hz.

**Note on this spec:** the downstream repo is Apache-2.0 (this one is MIT),
so this ask is specification and measurements only — no downstream code is
reproduced here. Everything below is describable from `std.mem`'s own
public surface.

## Symptom

A pure-Aether inner loop that composites sprites (read 4 source bytes,
read 4 destination bytes, blend in float, write 4 bytes — roughly 30–40
`std.mem` accessor touches per pixel) costs **~460ns per pixel**. Concrete
frames from the engine's benchmark, same machine, same gcc:

| Workload (one frame) | Pure Aether | Same loop as a C helper |
|---|---|---|
| 300 sprites of 32×32, source-over | 141 ms | 0.6 ms |
| One rotated 320×240 blit, bilinear | 27 ms | 3.8 ms |

A 60Hz game has a 16ms budget for *everything*, so the pure-Aether form is
~9× over budget before game logic runs. The downstream port had to keep one
C file containing only these inner loops; every other line of the engine is
Aether. We'd like to delete that file.

## Why it's slow (and why `-O2` doesn't save it)

`mem.get_byte` / `mem.set_byte` / `mem.get_float64` etc. each lower to a
call into `libaether.a`. The work inside is a couple of instructions; the
call is the cost. At ~10M+ accessor touches per frame this is tens of
millions of cross-TU function calls per second, and the C compiler cannot
inline them across the static-library boundary at any `-O` level (short of
LTO, which the toolchain doesn't do today). The same loop with direct
loads/stores vectorizes and/or reduces to `memcpy`-class throughput.

## Asks

1. **Lower the scalar `std.mem` accessors in codegen** instead of emitting
   extern calls: `get_byte`/`set_byte`, `get_int`/`set_int`,
   `get_long`/`set_long`, `get_float32`/`set_float32`,
   `get_float64`/`set_float64` (and the `_sz` twins). Semantics must be
   preserved exactly — including the documented null-`ptr` behaviour
   (read returns 0 / write returns 0) — an inline null test is a branch,
   not a call, and predicts perfectly in a loop. The extern symbols must
   remain in the runtime for existing binaries and FFI consumers; this is
   purely a call-site lowering.

2. **`mem.copy_at(dst, dst_off, src, src_off, n)`** (name yours). Aether
   deliberately has no pointer arithmetic, which means today there is no
   way to bulk-copy between *interior* spans of two buffers —
   `mem.copy(dst, src, n)` can only start at offset 0. Row-wise image
   operations (copy a scanline of a sub-region, the fast path of an
   opaque blit) are memcpy-shaped, and byte-at-a-time is the only current
   spelling. Same null/permissive contract as `mem.copy`.

3. *(Downgraded after measurement — see Acceptance)* An unchecked-access
   escape hatch in the spirit of `floatarr_get_unchecked`. Revisit only if
   a profile of ask 1's result shows the inline null branch is the
   remaining cost; the review benchmark (inlined-with-null-check 38ms vs
   raw-C 6ms per 60 frames, both far inside frame budget) says it isn't.

## Call-site census (downstream, by family)

- Per-pixel composite loops (byte accessors, ~30–40 per pixel): sprite
  blitting, rect fills, triangle rasterization with per-vertex color.
- PNG scanline unfiltering (byte accessors, sequential).
- Bitmap-font glyph blits (byte accessors).
- Interpolation scratch (float64 accessors, 4–16 per pixel).
- Small state tables — input snapshots, board games (byte/int accessors;
  not hot, but they'd ride along).

## Acceptance

A self-contained micro-benchmark, no downstream code needed: allocate two
640×480×4 byte buffers (via `std.arena` or a caller-supplied buffer —
`std.mem` itself has no allocator); per "frame", for every pixel read 4
bytes from each buffer, combine, write 4 back; run 60 frames.

Measured baseline for exactly that loop (Linux x86-64, gcc 12.2,
ae 0.578.0 — from the PR review, confirmed by objdump showing 8 `call
aether_mem_get_byte` instructions in the inner loop):

| | 60 frames |
|---|---:|
| Aether today (`std.mem` accessors) | ~215 ms |
| C `-O2`, inlined, null check kept | ~38 ms |
| C `-O2`, raw indexing | ~6 ms |

**Target: within ~6× of raw C — i.e. the cost of retaining the null
contract inline, ≈40 ms per 60 frames on a modern x86-64.** That is
checkable, and honest about what inlining can and cannot recover; the
null branch itself is the residual and is explicitly acceptable.

(The 141ms-per-frame figure in the Symptom table is a *different, heavier*
body — the downstream compositor touches ~30–40 accessors per pixel
including float64 scratch and blend math, where this acceptance loop
touches 12. Both shrink by the same mechanism; the acceptance bar is the
minimal loop above, with the measured baseline.)

`tests/` presumably wants that loop as a perf regression probe alongside
the correctness ones.

With asks 1+2 landed, the downstream engine folds its C helper back into
pure Aether behind an already-pinned test suite — the "runtime stays C"
line can retreat to where it belongs (devices, GUI toolkits, syscalls),
rather than covering arithmetic on bytes.
