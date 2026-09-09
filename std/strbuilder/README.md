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

## Scanning what you built (avoid the O(n²) trap)

`finish` returns a plain length-less string (a header-less `char*`, for a heap
ownership reason — see the note below). The catch: because it carries no length
header, `string.length`, `string.char_at` and `string.substring` each recompute
the length with `strlen` **on every call**. Scanning a finished string
character by character with those is therefore **O(n²)** — instant on a small
test input, a hang on a production-sized one, with correct results the whole
way (so it does not look like a bug).

If you are going to scan the result — a tokenizer, an interpreter, a template
renderer — take the length **once** and use the length-carrying accessors, or
finish with the length in hand:

```aether,fragment
// Slow: O(n²). Each char_at re-strlens the whole string.
s = strbuilder.finish(b)
n = string.length(s)
i = 0
while i < n { c = string.char_at(s, i); i = i + 1 }

// Fast: O(n). Cache the length once, pass it to the _n accessors.
s, n = strbuilder.finish_with_length(b)
i = 0
while i < n { c = string.string_char_at_n(s, n, i); i = i + 1 }
// string.string_substring_n(s, n, a, b) is the substring counterpart.
```

`finish_with_length` returns `(ptr, int)` and is also the binary-safe finish
(no NUL terminator appended), so it is the right default for both binary content
and scanning-heavy code.

## Exports

`new`, `append`, `append_n`, `append_byte`, `append_int`, `append_long`,
`append_hex`, `append_codepoint`, `append_format`, `length`, `capacity`,
`reserve`, `truncate`, `clear`, `finish`, `finish_with_length`, `free`.

`append_n` takes an explicit length, which is how a slice with embedded NUL
bytes gets appended in full rather than stopping at the first zero.
