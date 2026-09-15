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

## Scanning what you built

`finish` returns a length-carrying string: the builder writes into a block
laid out as an inline string from the first byte, and `finish` fills in the
header and hands the block over without copying. `string.length`,
`string.char_at` and `string.substring` read that length in O(1), so a
character-by-character scan of a finished string — a tokenizer, an
interpreter, a template renderer — is linear:

```aether,fragment
s = strbuilder.finish(b)
n = string.length(s)             // O(1)
i = 0
while i < n { c = string.char_at(s, i); i = i + 1 }   // O(n) overall
```

(Before this, `finish` returned a header-less buffer and every one of those
calls re-ran `strlen`, which made the same loop O(n²): instant on a test
input, a hang on a production-sized one, with the right answer throughout.)

Because the length travels with the string, content appended with `append_n`
that contains NUL bytes survives `finish` in full too.

`finish_with_length` returns `(ptr, int)`: a raw buffer with no NUL terminator
appended, for binary protocol assembly where the bytes are handed to C or
freed by the caller. It shifts the content down over the block's header, so
it costs one `memmove` of the content and no allocation.

## Exports

`new`, `append`, `append_n`, `append_byte`, `append_int`, `append_long`,
`append_hex`, `append_codepoint`, `append_format`, `length`, `capacity`,
`reserve`, `truncate`, `clear`, `finish`, `finish_with_length`, `free`.

`append_n` takes an explicit length, which is how a slice with embedded NUL
bytes gets appended in full rather than stopping at the first zero.
