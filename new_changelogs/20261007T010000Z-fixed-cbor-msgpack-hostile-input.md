- **`cbor.parse` and `msgpack.unpack` refuse nesting deeper than 256
  levels.** Both decoders recursed once per level with no bound, so a few
  hundred kilobytes of one-element arrays crashed the program with a stack
  overflow. They now stop at the same depth as `std.json` and return
  "nesting deeper than 256".
- **A length or count larger than the input is an error in `std.cbor` and
  `std.msgpack`.** Declared lengths were narrowed to a 32-bit int before the
  bounds check, so 2^31 and up came out negative and truncated input decoded
  as success: `5a80000000` as an empty CBOR byte string, `9b0000000100000000`
  as `[]`, MessagePack `db80000000` and `ddffffffff` as empty. Lengths are now
  checked whole against the bytes left, and a count those bytes could not
  hold is refused before anything is read or allocated for it.
- **Unsigned 64-bit integers above the signed range are refused instead of
  flipping sign.** CBOR `1bffffffffffffffff` and MessagePack
  `cfffffffffffffffff` (2^64-1) both decoded as -1, and CBOR negative
  integers below -2^63 wrapped. Values the signed 64-bit getters cannot hold
  now return "integer out of range".
- **`cbor.set` replaces an existing key's value.** It appended a second
  entry with the same key, so `object_get` kept returning the old value and
  the encoded map carried a duplicate key; it now replaces and frees the old
  value, as `json.set` and `msgpack.map_set` do.
