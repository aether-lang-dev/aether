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

## Crossing between `ptr` and `string`

There is deliberately no `ptr as string` cast (they are different types).
`std.bytes` is the bridge. The two crossings, each now one call:

```aether,fragment
// ptr -> string  (I have a raw buffer + length, I want a binary string)
s = bytes.string_from_ptr(p, n)

// string -> ptr  (I have a string, I want a raw pointer to walk)
n = string.length(s)
b = bytes.new(n)
_ = bytes.copy_from_string(b, 0, s, n)
p = bytes.data(b)
// ... use p ...  (keep `b` alive as long as you use `p`)
```

The longhand still works and is useful when you build a buffer incrementally:

```aether,fragment
b = bytes.new(n)
p = bytes.data(b)               // raw region, already zero-filled to capacity
// ... write n bytes through p ...
s = bytes.finish(b, n)          // hand off the first n bytes as a string
```

Note: `finish(b, n)` / `to_string(b, n)` honour the `n` you pass up to the
buffer's **capacity**, so writing directly through `data()` and finishing with
the count works without a preceding `set_length`. (Before #1782 the count was
silently clamped to the logical length, so a direct write finished to an empty
string; that trap is gone.) `set_length` is still the way to publish a length
for `bytes.length`/`get`/`copy_*` to see.

## Slices (#2301)

A buffer handle is not its data, and `data()` is a raw pointer with no
length. `view(b)` gives the live contents as a `byte[]` bounded by
`length(b)`, and `capacity_view(b)` the writable storage bounded by
`capacity(b)`, so an index past either end is the slice bounds panic rather
than a read past the buffer. Both borrow: they are valid until the next call
that can grow the buffer (which may move its storage) or `free`.

The slice-taking helpers need no separate count: `from_slice(s)` makes a
buffer holding a copy of `s`, `copy_from_slice(b, dst, s)` copies `s` in at
an offset (growing the buffer, and reading `s` correctly even when it is a
view of the same buffer that the growth moves), and `string_from_slice(s)`
makes a binary-safe string. A slice with no bound (`p as byte[]`) panics.

```aether,run
import std.bytes

main() {
    b = bytes.new(16)
    w = bytes.capacity_view(b)       // write straight into the storage...
    w[0] = 72
    w[1] = 105
    bytes.set_length(b, 2)           // ...then publish how much is live
    v = bytes.view(b)
    println("${v.len} bytes: ${bytes.string_from_slice(v)}")
    bytes.copy_from_slice(b, 2, v)   // append the buffer to itself
    println(bytes.string_from_slice(bytes.view(b)))
    bytes.free(b)
}
```
```output
2 bytes: Hi
HiHi
```

`std.bytes.cursor` reads slices too: `cursor.from_slice(s)` makes a cursor
over any bounded `byte[]` (a sub-slice of a view, a fixed array), and
`cursor.read_view(c, n)` returns the next `n` bytes as a borrowed sub-slice
without copying, where `read_slice` copies them into a new buffer.

## Exports

`new`, `from_ptr`, `string_from_ptr`, `set`, `get`, `length`, `set_length`,
`capacity`, `data`, `finish`, `to_string`, `free`, `copy_from_string`,
`copy_from_bytes`, `copy_within`, the slice forms `view`, `capacity_view`,
`from_slice`, `copy_from_slice`, `string_from_slice`, and the fixed-width
endian accessors `set_le16`/`get_le16` through `set_be64`/`get_be64`.

The endian accessors are the reason to reach for a `bytes` buffer over a
string when building a wire format: `set_be32` writes the four bytes in
network order without the caller shifting and masking.
