# std.strbuilder

Amortised O(1) string building.

Concatenating in a loop with `string.concat` is quadratic: each step copies
everything accumulated so far. A builder appends into a growing buffer and
copies once, at `finish`. For a few pieces it does not matter; for a loop over
a thousand rows it is the difference between instant and noticeable.

`new` takes a capacity hint. Getting it wrong is not an error — the buffer
grows — but a reasonable guess avoids the regrowth.

```aether,run
import std.strbuilder

main() {
    sb = strbuilder.new(64)

    strbuilder.append(sb, "Hello")
    strbuilder.append(sb, ", ")
    strbuilder.append(sb, "world")

    // finish consumes the builder and hands back the string.
    println(strbuilder.finish(sb))
}
```
```output
Hello, world
```

`finish` takes ownership: the builder must not be used afterwards. To keep
building, read `length` as you go and call `finish` once at the end.

## Exports

`new`, `append`, `append_n`, `append_byte`, `append_int`, `append_long`,
`append_hex`, `append_codepoint`, `append_format`, `length`, `capacity`,
`reserve`, `truncate`, `clear`, `finish`, `finish_with_length`, `free`.

`append_n` takes an explicit length, which is how a slice with embedded NUL
bytes gets appended in full rather than stopping at the first zero.
