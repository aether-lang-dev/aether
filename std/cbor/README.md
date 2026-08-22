# std.cbor

CBOR (RFC 8949): a binary format with JSON's data model, plus tags.

Same shapes as JSON — but self-describing, compact, and with a **tag**
mechanism that lets a value carry its semantic type. That is what makes CBOR
the encoding under COSE, WebAuthn and much of the constrained-device world:
a timestamp can be tagged as a timestamp rather than agreed by convention.

`diagnose` renders a value in CBOR's diagnostic notation, which is the
readable form used in specs and test vectors — invaluable when comparing
against one.

```aether,run
import std.cbor
import std.encoding
import std.string

main() {
    value = cbor.from_int(42)

    encoded, eerr = cbor.encode(value)
    // 0x18 introduces a one-byte unsigned; 0x2a is 42.
    println("hex: ${encoding.hex_encode(encoded, string.length(encoded))} err='${eerr}'")

    back, perr = cbor.parse(encoded)
    text, derr = cbor.diagnose(back)
    println("diagnostic: ${text} err='${perr}'")

    cbor.free(value)
    cbor.free(back)
}
```
```output
hex: 182a err=''
diagnostic: 42 err=''
```

Values are built with `from_int`, `num`, `str`, `arr`, `obj`, `boolean` and
`null_value`, and every constructed value is freed with `cbor.free` — freeing
a container frees what it holds.

Compared with `std.msgpack`: both are binary JSON-shaped formats, and MessagePack
is slightly more compact for small integers. CBOR is the one with an RFC, tags,
and the surrounding standards, so pick it when interoperating with anything that
specifies it.

## Exports

`parse`, `encode`, `diagnose`, `from_int`, `num`, `str`, `arr`, `obj`,
`boolean`, `null_value`, `free`, and the `CBOR_*` type constants.
