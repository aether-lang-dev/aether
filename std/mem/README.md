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

Use `alloc_aligned` when the wider accessors are involved: `set_long` and the
u64 pair at an unaligned offset are undefined on strict-alignment targets even
where x86 tolerates them.

`get_uint32` and `set_uint32` take and return `long`, so the full unsigned
range works without masking — they used to be `int`, which made `0xFFFFFFFF`
read back as -1 (#1699). `set_uint32` truncates a wider value to the field
rather than spilling into the next one.

`bits_of_float` / `float_from_bits` reinterpret a double as its IEEE 754 bit
pattern and back, without going through a conversion.

## Exports

`get_byte`, `set_byte`, `get_int`, `set_int`, `get_long`, `set_long`,
`get_int8`, `set_int8`, `get_uint8`, `set_uint8`, `get_int16`, `set_int16`,
`get_uint16`, `set_uint16`, `get_uint32`, `set_uint32`, `get_float32`,
`set_float32`, `get_float64`, `set_float64`, `get_ptr`, `set_ptr`; the
endian pairs `get_u16_le` through `set_u64_be`; `bits_of_float`,
`float_from_bits`, `clz32`, `clz64`, `udiv64_32`; `copy`, `move`, `compare`,
`set`.

`mem.ptr_to_long` and `mem.long_to_ptr` are defined and callable but are
**missing from the module's `exports(...)` list**. They work today because a
whole-module import resolves them anyway; treat that as an oversight to be
fixed rather than a guarantee.
