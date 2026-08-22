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
    println("packed: ${encoding.hex_encode(packed, string.length(packed))}")

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
freeing a container frees what it holds.

`get_type` returns the type tag, so a reader dispatches on the wire type
rather than assuming — the same discipline `std.json`'s `json_type` asks for.

## Exports

`nil_value`, `boolean`, `from_int`, `num`, `str`, `bin`, `arr`, `map`, `ext`,
`pack`, `unpack`, `get_type`, `get_bool`, `get_int`, `get_float`, `get_string`,
`get_bin`, `array_size`, `array_get`, `array_add`, `map_size`,
`map_get`, `map_set`, `map_get_key`, `map_get_value`, `free`.
