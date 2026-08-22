# std.tsid

TSID: a 13-character, time-sortable identifier in Crockford base32.

A 64-bit value — 42 bits of millisecond timestamp from a 2020 epoch, shifted
left over 22 random bits. It is the most compact of the sortable identifiers
here: 13 characters against ULID's 26 and UUID's 36, at the cost of fewer
random bits per millisecond.

```aether,run
import std.tsid
import std.string

main() {
    id, err = tsid.generate()
    println("length: ${string.length(id)} err='${err}'")

    // 42 bits of timestamp in the high bits leaves the leading
    // character at '0' until roughly the year 2160.
    println("leading: ${string.substring(id, 0, 1)}")
}
```
```output
length: 13 err=''
leading: 0
```

22 random bits per millisecond is roughly four million values — ample for one
process, but the collision probability is not negligible if many processes
mint IDs in the same millisecond without coordination. `std.ulid`'s 80 random
bits are the safer default at scale.

## Exports

`generate`.
