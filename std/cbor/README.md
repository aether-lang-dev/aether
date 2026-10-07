# std.cbor

CBOR (RFC 8949): a binary format with JSON's data model, plus tags.

Same shapes as JSON — but self-describing, compact, and with a **tag**
mechanism that lets a value carry its semantic type. That is what makes CBOR
the encoding under COSE, WebAuthn and much of the constrained-device world:
a timestamp can be tagged as a timestamp rather than agreed by convention.

`diagnose` renders a value in CBOR's diagnostic notation (RFC 8949 §8),
which is the readable form used in specs and test vectors, and invaluable
when comparing against one. It writes what the value is exactly: floats in
the shortest decimal that reads back as the same number (`1.1`,
`5.960464477539063e-8`, `1.0e+300`, `NaN`, `-Infinity`), text with JSON's
escapes, tags as `24(h'...')`, `simple(n)`, and the §8.1 encoding
indicators wherever a parsed item did not use preferred serialization: `_`
for an indefinite length (`[_ 1, 2]`, `(_ h'01', h'02')`, `''_`) and `_n`
for an argument in 2^n bytes that would fit in fewer (`1.5_3` is a 1.5
encoded as a double, `0_0` is 0 in two bytes). Without the `_n` indicators,
which the RFC's Appendix A leaves out, every example of that appendix
diagnoses as the RFC prints it.

```aether,run
import std.cbor
import std.encoding
import std.string

main() {
    value = cbor.from_int(42)

    encoded, eerr = cbor.encode(value)
    // 0x18 introduces a one-byte unsigned; 0x2a is 42.
    println("hex: ${encoding.hex_encode(string.bytes(encoded))} err='${eerr}'")

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

`set` (`object_set`) on a key the map already has replaces its value and
frees the old one, as `json.set` does. `map_get` finds a key of any type by
content, arrays and maps included (a map's pairs in any order).

A parsed value remembers how it was encoded, so `encode` writes it back byte
for byte: float widths, NaN payloads, indefinite lengths with their chunks,
and argument sizes. A value built with the constructors encodes in preferred
serialization: the shortest head, and a float in the narrowest width that
holds it exactly (`num(1.5)` is `f93e00`).

`parse` treats its input as untrusted, and reads one data item that must be
the whole input. It refuses, with an error rather than a value:

- bytes after the item (`cbor: trailing bytes after the data item`): `0102`
  is not 1;
- anything that is not well-formed (RFC 8949 §3, every case of Appendix F):
  a reserved additional-information value, a break outside an
  indefinite-length item or where a map value belongs
  (`cbor: unexpected break`), an indefinite-length string chunk that is not a
  definite-length string of the same type, a simple value below 32 in two
  bytes, a truncated head or body;
- a text string, or a text chunk, that is not UTF-8
  (`cbor: invalid UTF-8 in text string`);
- nesting deeper than 256 levels, the same limit as `std.json`
  (`cbor: nesting deeper than 256`);
- a string length, or an array or map count, larger than the bytes left in
  the input (`cbor: unexpected EOF`);
- an integer, length or tag number of 2^63 or more, which the signed 64-bit
  `get_long` cannot hold, and a negative integer below -2^63
  (`cbor: integer out of range`).

Compared with `std.msgpack`: both are binary JSON-shaped formats, and MessagePack
is slightly more compact for small integers. CBOR is the one with an RFC, tags,
and the surrounding standards, so pick it when interoperating with anything that
specifies it.

## Exports

`parse`, `encode`, `diagnose`, `from_int`, `num`, `str`, `arr`, `obj`,
`boolean`, `null_value`, `free`, and the `CBOR_*` type constants.
