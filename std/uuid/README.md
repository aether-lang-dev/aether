# std.uuid

UUID v4 and v7 (RFC 9562), both in the canonical 36-character
`8-4-4-4-12` hex form.

**v4** is 122 random bits — the classic opaque identifier. **v7** puts a
48-bit millisecond timestamp in the leading bytes, so v7 values sort by
creation time as plain strings. For a database primary key that matters:
v4 keys scatter across a B-tree and fragment it, v7 keys append.

Both return `(uuid, err)` and can only fail if the CSPRNG does.

```aether,run
import std.uuid
import std.string

main() {
    v4, err4 = uuid.v4()
    v7, err7 = uuid.v7()

    // The values are random, so assert the shape rather than the text.
    println("v4 length: ${string.length(v4)} err='${err4}'")
    println("v7 length: ${string.length(v7)} err='${err7}'")

    // Position 14 carries the version nibble — the one visible
    // difference between the two forms.
    println("v4 version: ${string.substring(v4, 14, 15)}")
    println("v7 version: ${string.substring(v7, 14, 15)}")
}
```
```output
v4 length: 36 err=''
v7 length: 36 err=''
v4 version: 4
v7 version: 7
```

v7 carries no monotonic counter, so two IDs minted in the same millisecond
sort arbitrarily *within* that millisecond. Across milliseconds the ordering
holds. A caller needing strict per-process ordering should layer a counter on
top.

## Exports

`v4`, `v7`.
