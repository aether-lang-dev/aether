# std.encoding

Hex, Base64, Base32 and CSV field splitting.

Every encoder takes an explicit **length** alongside the data, because Aether
strings are length-carrying and may contain NUL bytes — passing a length means
a binary payload encodes correctly rather than stopping at the first zero.

Decoders return `string!`, so a malformed input is a value you must handle
rather than a silent empty result. `or { }` is the idiomatic form.

```aether,run
import std.encoding
import std.string

main() {
    data = "Ab"
    println(encoding.hex_encode(string.bytes(data)))

    // Decoders are fallible: handle the failure or propagate it.
    back = encoding.hex_decode("4162") or {
        println("not valid hex")
        return
    }
    println(back)
}
```
```output
4162
Ab
```

## Base64

`base64_encode` emits unpadded output; `base64_encode_padded` adds the `=`
tail. The decoder accepts either.

```aether,run
import std.encoding
import std.string

main() {
    msg = "hello"

    println(encoding.base64_encode(string.bytes(msg)))
    println(encoding.base64_encode_padded(string.bytes(msg)))

    decoded = encoding.base64_decode("aGVsbG8") or {
        println("not valid base64")
        return
    }
    println(decoded)
}
```
```output
aGVsbG8
aGVsbG8=
hello
```

## Base32

`base32_encode` emits the standard RFC 4648 alphabet with `=` padding. The
decoder accepts padded or unpadded input in either case, and rejects what no
encoder produces: a final group of 1, 3 or 6 characters, padding that does not
complete the last group to 8, or non-zero bits after the last byte.

## Exports

`hex_encode`, `hex_decode`, `base64_encode`, `base64_encode_padded`,
`base64_decode`, `base32_encode`, `base32_decode`, `csv_split`, `csv_count`,
`csv_field`, `csv_free`.
