# std.mem

Typed reads and writes over raw memory.

Signed and unsigned accessors at every width, little- and big-endian pairs for
u16/u32/u64, float storage and bit reinterpretation, plus `copy`, `move` and
`compare`. This is what a codec, a binary parser or a port of C code uses to
get at bytes.

**`std.mem` has no allocator.** It reads and writes memory someone else owns —
`std.arena` is the usual source. Note that a `std.bytes` handle is a *struct*,
not a pointer to its payload: writing through one with these accessors
overwrites the struct's own fields and corrupts the heap.

```aether,run
import std.mem
import std.arena

main() {
    ar = arena.create(256)
    p = arena.alloc_aligned(ar, 32, 8)
    for (i = 0; i < 32; i = i + 1) {
        mem.set_uint8(p, i, 0)
    }

    // Endianness is explicit: 0x12345678 little-endian puts 0x78 first.
    mem.set_u32_le(p, 0, 305419896)
    println("byte 0: ${mem.get_uint8(p, 0)}")
    println("byte 3: ${mem.get_uint8(p, 3)}")
    println("read back: ${mem.get_u32_le(p, 0)}")

    // Signedness is the accessor's, not the memory's: the same byte
    // reads as -1 or 255 depending on which you ask for.
    mem.set_int8(p, 8, -1)
    println("as int8: ${mem.get_int8(p, 8)}, as uint8: ${mem.get_uint8(p, 8)}")

    arena.destroy(ar)
}
```
```output
byte 0: 120
byte 3: 18
read back: 305419896
as int8: -1, as uint8: 255
```

Every accessor here takes a **byte offset**, whatever its width:
`get_float32(p, 4)` reads the second float, not the fifth. (`std.lanes`, the
other module that reaches into a raw buffer, indexes by **element** — the
convention an `f32[]` view uses.)

Use `alloc_aligned` when the wider accessors are involved: `set_long` and the
u64 pair at an unaligned offset are undefined on strict-alignment targets even
where x86 tolerates them.

`get_uint32` and `set_uint32` take and return `long`, so the full unsigned
range works without masking — they used to be `int`, which made `0xFFFFFFFF`
read back as -1 (#1699). `set_uint32` truncates a wider value to the field
rather than spilling into the next one.

`bits_of_float` / `float_from_bits` reinterpret a double as its IEEE 754 bit
pattern and back, without going through a conversion.

The scalar accessors cost no more than the load or store they wrap. The
compiler emits the native-endian `get_*` / `set_*` accessors (`byte`, `int8`
through `uint32`, `int`, `long`, `float32`, `float64`, `ptr`) and
`ptr_to_long` / `long_to_ptr`, `bits_of_float` / `float_from_bits`, and
`clz32` / `clz64` inline in the calling translation unit rather
than as calls into libaether (the `_le` / `_be` pairs are still calls), with the same null behaviour as the library
functions (a null read gives 0, or -1 for `get_byte`; a null write gives 0).
A port that walks a heap through these in its inner loop, as mquickjs-ae's VM
does, does not pay a call per access.

For a hot loop whose pointers are known to be non-null, each native-endian
scalar getter and setter also has an inline `_unchecked` companion:
`get_byte_unchecked`, `set_byte_unchecked`, `get_byte_sz_unchecked`,
`set_byte_sz_unchecked`, and `get_*_unchecked` / `set_*_unchecked` for
`ptr`, `int`, `long`, `int8`, `uint8`, `int16`, `uint16`, `uint32`,
`float32` and `float64`. These omit the null check; passing null is undefined
behavior. Width, truncation, alignment and byte-offset requirements match
the checked forms, and setters return 1. Pointer loads/stores retain the
alias-safe memcpy lowering. `--audit-mem` reports these accesses too.
`clz32` and `clz64` require a nonzero value in both the inline and extern forms.

## Bulk operations on interior spans

`copy`, `move`, `compare` and `set` all start at byte 0 of each buffer. Since
Aether has no pointer arithmetic, that left no way to express a bulk
operation on an **interior** span — copying one scanline out of a larger
framebuffer meant going byte at a time, even though the operation is
memcpy-shaped.

`copy_at`, `move_at`, `fill_at` and `compare_at` take explicit offsets:

```aether,run
import std.mem
import std.arena

main() {
    ar = arena.create(256)
    src = arena.alloc(ar, 32)
    dst = arena.alloc(ar, 32)
    i = 0
    while i < 32 { _ = mem.set_byte(src, i, i) ; i = i + 1 }

    // copy src[8..12) into dst[20..24)
    _ = mem.copy_at(dst, 20, src, 8, 4)
    println("${mem.get_byte(dst, 20)} ${mem.get_byte(dst, 23)}")
    arena.destroy(ar)
}
```
```output
8 11
```

Contracts match the offset-less forms exactly: a null buffer is a no-op
returning `dst` (`0` for `compare_at`), and offsets are the caller's
responsibility in the same way `n` already is — no range checking is added,
because none exists in the offset-less forms.

**Use `move_at`, not `copy_at`, when the spans overlap.** An image scrolling
within its own buffer is an overlapping interior copy; `copy_at` is `memcpy`
and is undefined there, exactly as in C.

## How much the heap holds

`mem.heap_in_use()` is the bytes the C allocator has handed out and not taken
back, read from the allocator's own statistics: every malloc in the process
counts, Aether's (`heap.new`, strings, closure environments, collections) and
the ones C code reached through an extern makes, so a same-process leak
check can run locally and in seconds, not only under a leak tool on one CI
leg.

Run the workload a few rounds and compare two later rounds: the growth is
what a round leaks. A single before/after is not zero even when nothing
leaks. On Windows (a walk of the process's heaps) and under a sanitizer's
allocator the count is exactly the blocks the program holds, and
`mem.heap_in_use_exact()` is true: between steady rounds the growth is zero
unless something leaks. glibc, macOS and FreeBSD report from allocator
statistics that also count freed blocks parked in per-thread caches; on a
workload that churns many allocations those settle over many rounds, so two
rounds there can differ by a few kilobytes without a leak. A Windows program
running under Wine is in the same position: Wine's heap walk counts a
low-fragmentation group as one block of its whole size, however few of its
slots are in use. Where
`heap_in_use_exact()` is false, a leak check that must not misfire belongs to
a leak tool (`leaks`, valgrind, LeakSanitizer).

```aether,run
import std.mem

struct Blob {
    words: long[32]
}

extern malloc(n: long) -> ptr
extern free(p: ptr)

round(keep: ptr, leak: bool) {
    i = 0
    while i < 100 {
        b = heap.new(Blob)
        if leak {
            mem.set_ptr(keep, i * 8, b)
        } else {
            free(b)
        }
        i = i + 1
    }
}

main() {
    keep = malloc(800)
    round(keep, false)
    round(keep, false)
    steady = mem.heap_in_use()
    round(keep, false)
    println("no leak: ${mem.heap_in_use() - steady < 4096}")

    before = mem.heap_in_use()
    round(keep, true)
    println("leaked at least 100 blocks: ${mem.heap_in_use() - before >= 12800}")

    i = 0
    while i < 100 {
        free(mem.get_ptr(keep, i * 8))
        i = i + 1
    }
    free(keep)
}
```
```output
no leak: true
leaked at least 100 blocks: true
```

The source per platform: glibc `mallinfo2`, macOS's malloc zones, Windows
`HeapWalk` over the process's heaps, FreeBSD jemalloc's
`stats.allocated`, and the sanitizer's allocator when built with one. It is
`-1` where the allocator keeps no statistics (musl), and where the platform's
allocator is not the one serving malloc (under valgrind, or with an allocator
preloaded in front of glibc's), since its numbers would stand still. For
what the OS charges the process, page-granular and including mappings the
allocator does not see, `std.os` has `memory_resident()` and
`memory_private()`.

## Fields in a `byte[]` (#2301)

The pointer accessors trust every offset. When the bytes are a slice (a
`std.bytes` buffer's `view()`, a sub-slice, a fixed array), the slice forms
check that the whole field lies inside it, and a field that does not is a
panic naming the offset, the width and the length:

```aether,fragment
v = bytes.view(frame)
kind = mem.read_u16_be(v, 0)
size = mem.read_u32_le(v, 2)      // panics if fewer than 6 bytes
mem.write_u64_be(v, 8, stamp)
mem.copy_slice(v[16..], payload)  // overlap-safe; panics if it does not fit
mem.fill_slice(v[40..48], 0)
```

`read_u16_le` through `write_u64_be` have the widths and byte order of
`get_u16_le` through `set_u64_be`. `compare_slice(a, b)` is `compare`'s
slice form: lexicographic, comparing the overlapping prefix first and, if
every shared byte matches, the shorter slice sorting first — two
different-length slices are therefore never "equal" even when one is a
prefix of the other.

## Exports

`get_byte`, `set_byte`, `get_int`, `set_int`, `get_long`, `set_long`,
`get_int8`, `set_int8`, `get_uint8`, `set_uint8`, `get_int16`, `set_int16`,
`get_uint16`, `set_uint16`, `get_uint32`, `set_uint32`, `get_float32`,
`set_float32`, `get_float64`, `set_float64`, `get_ptr`, `set_ptr`; the
endian pairs `get_u16_le` through `set_u64_be`; `bits_of_float`,
`float_from_bits`, `clz32`, `clz64`, `udiv64_32`; `copy`, `move`, `compare`,
`set`, `copy_at`, `move_at`, `fill_at`, `compare_at`; `get_byte_sz`,
`set_byte_sz`; `ptr_to_long`, `long_to_ptr`; `call_fn3_int`, `call_fn3_void`,
`call_fn2_void`; `heap_in_use`, `heap_in_use_exact`; the slice forms `read_u16_le` through
`write_u64_be`, `copy_slice`, `fill_slice` and `compare_slice`.
