# std.ulid

ULID: a 26-character, time-sortable identifier in Crockford base32.

48 bits of millisecond timestamp followed by 80 random bits. Two properties
follow: ULIDs generated at different times sort by creation time as plain
strings, and the alphabet excludes `I`, `L`, `O` and `U` so a transcribed ID
cannot be misread as a digit.

```aether,run
import std.ulid
import std.string

main() {
    id, err = ulid.generate()
    println("length: ${string.length(id)} err='${err}'")

    // Crockford base32 omits the letters that look like digits.
    println("has I: ${string.contains(id, "I")}")
    println("has O: ${string.contains(id, "O")}")
}
```
```output
length: 26 err=''
has I: 0
has O: 0
```

Within a single millisecond the ordering is by the random tail, so
same-millisecond ULIDs sort arbitrarily relative to each other. Across
milliseconds the ordering holds.

Compared with `std.uuid`'s v7 — same idea, different encoding: ULID is 26
characters of base32, UUID v7 is 36 of hex-with-hyphens and fits anywhere a
UUID is already expected.

## Exports

`generate`.
