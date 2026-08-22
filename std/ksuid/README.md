# std.ksuid

KSUID: a 27-character, time-sortable identifier in base62.

A 4-byte second-resolution timestamp (offset from a 2014 epoch) followed by
16 random bytes, rendered in `0-9A-Za-z`. Base62 means no hyphens,
underscores or case-ambiguity concerns — it is alphanumeric throughout, which
is what makes it safe in a path segment or a double-click selection.

```aether,run
import std.ksuid
import std.string

main() {
    id, err = ksuid.generate()
    println("length: ${string.length(id)} err='${err}'")

    a, _ = ksuid.generate()
    b, _ = ksuid.generate()
    println("distinct: ${string.equals(a, b) == 0}")
}
```
```output
length: 27 err=''
distinct: true
```

The timestamp has **second** resolution, so IDs minted within the same second
sort by their random tail rather than by time. If sub-second ordering matters,
`std.ulid` or `std.uuid`'s v7 use millisecond timestamps.

## Exports

`generate`.
