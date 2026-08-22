# std.nanoid

URL-safe random identifiers, 21 characters by default.

The alphabet is 64 symbols — `A-Za-z0-9_-` — so a NanoID drops into a URL,
a filename or a header without escaping. 21 characters of it is ~126 bits of
randomness, comparable to a UUID v4 in 15 fewer characters and with no
hyphens.

```aether,run
import std.nanoid
import std.string

main() {
    id, err = nanoid.generate()
    println("default length: ${string.length(id)} err='${err}'")

    // generate_n for a shorter (or longer) id. Shorter means fewer
    // bits: 8 characters is 48 bits, fine for a cache key and not for
    // anything an attacker gets to guess at.
    short, serr = nanoid.generate_n(8)
    println("short length: ${string.length(short)} err='${serr}'")

    // A length below 1 is rejected rather than silently clamped.
    _, berr = nanoid.generate_n(0)
    println("generate_n(0): '${berr}'")
}
```
```output
default length: 21 err=''
short length: 8 err=''
generate_n(0): 'n must be >= 1'
```

## Exports

`generate`, `generate_n`.
