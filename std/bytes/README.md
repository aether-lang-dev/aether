# std.bytes

A growable byte buffer, for assembling binary data.

Aether strings are length-carrying and may contain NUL, so a buffer built here
converts to a string that survives binary content intact — which is what makes
`std.bytes` the right way to build a packet, an encoded frame or a file image.

**`bytes.new(n)` does not zero its allocation.** Reading a slot you have not
written returns whatever the allocator was holding. Set what you mean to read,
or clear the buffer first.

```aether,run
import std.bytes

main() {
    b = bytes.new(4)

    bytes.set(b, 0, 65)   // 'A'
    bytes.set(b, 1, 66)   // 'B'

    // finish takes the length you actually wrote, not the capacity.
    println(bytes.finish(b, 2))
}
```
```output
AB
```

`finish` consumes the buffer. `to_string` is the non-consuming form, for when
the buffer is still being filled.

A `bytes` handle is a struct, **not** a pointer to its payload — writing
through it with `std.mem`'s raw accessors overwrites the struct's own fields
and corrupts the heap. Use `bytes.set`/`bytes.get`, or allocate through
`std.arena` when you need raw `std.mem` access.

## Exports

`new`, `set`, `get`, `length`, `set_length`, `capacity`, `data`, `finish`,
`to_string`, `free`, `copy_from_string`, `copy_from_bytes`, `copy_within`,
and the fixed-width endian accessors `set_le16`/`get_le16` through
`set_be64`/`get_be64`.

The endian accessors are the reason to reach for a `bytes` buffer over a
string when building a wire format: `set_be32` writes the four bytes in
network order without the caller shifting and masking.
