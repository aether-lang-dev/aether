# std.msgpack

MessagePack: a binary serialisation format with JSON's data model.

Same shapes as JSON — nil, bool, number, string, binary, array, map — encoded
compactly. Small integers cost one byte, and there is a real `bin` type, so a
byte payload does not need base64'ing the way it does in JSON.

Values are built with constructors (`from_int`, `str`, `arr`, `map`), packed
with `pack`, and read back with `unpack`, which returns `(value, err)`.

```aether,run
import std.msgpack
import std.encoding
import std.string

main() {
    value = msgpack.from_int(42)

    packed = msgpack.pack(value)
    // 42 fits MessagePack's positive fixint: one byte, no header.
    println("packed: ${encoding.hex_encode(string.bytes(packed))}")

    back, err = msgpack.unpack(packed)
    println("err='${err}' value=${msgpack.get_int(back)}")

    msgpack.free(value)
    msgpack.free(back)
}
```
```output
packed: 2a
err='' value=42
```

Every constructed value is owned by the caller and freed with `msgpack.free`;
freeing a container frees what it holds. `map_set` on a key the map already
has frees the old value, unless it is the very value being set.

`get_type` returns the type tag, so a reader dispatches on the wire type
rather than assuming — the same discipline `std.json`'s `json_type` asks for.

`unpack` treats its input as untrusted, and reads one value that must be
the whole input. It refuses, with an error rather than a value:

- bytes after the value (`trailing bytes after value`): `0102` is not 1;
- nesting deeper than 256 levels, the same limit as `std.json`
  (`nesting deeper than 256`);
- a str, bin or ext length, or an array or map count, larger than the bytes
  left in the input (`EOF reading ...`).

A uint 64 of 2^63 or more unpacks, and packs back as a uint 64. `get_int`
holds the signed 64-bit part and gives 0 for it; `get_int64` returns the
same value with an error (`integer outside the signed 64-bit range`) when
it does not fit, and `get_uint` reads any non-negative integer as the bit
pattern of a long, the way std passes an unsigned 64-bit value (`std.bits`'
`udiv64`, `urem64` and `ucmp64` treat it as unsigned). `from_uint` builds
one.

## Exports

`nil_value`, `boolean`, `from_int`, `num`, `str`, `bin`, `arr`, `map`, `ext`,
`from_uint`, `pack`, `unpack`, `get_type`, `get_bool`, `get_int`, `get_int64`,
`get_uint`, `get_float`, `get_string`,
`get_bin`, `get_ext_type`, `array_size`, `array_get`, `array_add`, `map_size`,
`map_get`, `map_set`, `map_get_key`, `map_get_value`, `free`.
