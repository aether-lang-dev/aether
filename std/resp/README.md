# std.resp

RESP, the Redis Serialization Protocol: RESP3-native, RESP2-compatible.

RESP3 is a strict **superset** of RESP2 — every RESP2 marker (`+ - : $ *`) is
valid RESP3, which merely adds types (null `_`, boolean `#`, double `,`, big
number `(`, verbatim string `=`, map `%`, set `~`, push `>`). So this is one
codec, not two protocols: a single decoder that reads the full RESP3 grammar
(and therefore any RESP2 stream), and a single encoder whose dialect flag
selects the wire form for the handful of types that differ. RESP3 is opt-in per
connection (a client sends `HELLO 3`), which is exactly why a client or server
must still be able to speak RESP2 — so `encode` emits RESP3 and `encode_resp2`
emits the RESP2 form of the same value.

Transport-agnostic, like `std.json` and `std.cbor`: it works over byte buffers
and never touches a socket. The decoder is **resumable** — `parse_prefix`
distinguishes *incomplete* (needs more bytes: it returns a null value, `0`
consumed, and no error) from *malformed* (a protocol error: a non-empty error
string). A caller reading a socket can accumulate bytes and retry a partial
frame instead of mis-framing it.

```aether,run
import std.resp
import std.string

main() {
    // Build a request: *2 ["GET", "mykey"]
    req = resp.new_array()
    resp.array_add(req, resp.new_bulk("GET", 3))
    resp.array_add(req, resp.new_bulk("mykey", 5))

    wire, _e = resp.encode_resp2(req)   // RESP2 dialect, the client default
    println("wire: ${string.replace_all(wire, "\r\n", "\\r\\n")}")
    resp.free_value(req)

    // Decode a RESP3 reply: a map {"speed" => 42}
    msg, consumed, perr = resp.parse_prefix("%1\r\n$5\r\nspeed\r\n:42\r\n", 20)
    println("map size: ${resp.map_size(msg)} consumed: ${consumed} err='${perr}'")
    println("speed = ${resp.as_integer(resp.map_value(msg, 0))}")
    resp.free_value(msg)
}
```
```output
wire: *2\r\n$3\r\nGET\r\n$5\r\nmykey\r\n
map size: 1 consumed: 20 err=''
speed = 42
```

## Building values

Scalars have direct constructors: `new_simple`, `new_error`, `new_integer`,
`new_bulk` (binary-safe — pass the byte length), `new_null`, `new_bool`,
`new_double`. Aggregates start empty and take children: `new_array` / `new_set`
/ `new_push` with `array_add`, and `new_map` with `map_add`. Ownership moves
**into** the aggregate — freeing a value you have already added, then freeing
its parent, is a double free. Every value you still own is freed with
`free_value`, which reclaims the whole tree.

## Reading values

`value_type` returns one of the `RESP_*` tags. `is_null` folds both the RESP3
`_` and the RESP2 `$-1` / `*-1` null forms. `as_integer`, `as_bool`,
`as_double` and `str_value` read scalars (bulk strings are binary-safe, so
`str_value` returns the payload as an owned string). Aggregates are read with
`array_size` / `array_get` and `map_size` / `map_key` / `map_value`.

## Decoding a stream

`parse` decodes exactly one complete value from a buffer and errors on trailing
bytes — use it for a single self-contained frame. `parse_prefix` decodes the
first value from a buffer that may hold more (or less) than one frame, returning
how many bytes it consumed; loop on it, advancing by `consumed`, and stop when
it reports incomplete to wait for more socket data.

## Exports

The `RESP_*` type tags; constructors `new_simple`, `new_error`, `new_integer`,
`new_bulk`, `new_null`, `new_bool`, `new_double`, `new_array`, `new_map`,
`new_set`, `new_push`; builders `array_add`, `map_add`; accessors `value_type`,
`is_null`, `as_integer`, `as_bool`, `as_double`, `str_value`, `array_size`,
`array_get`, `map_size`, `map_key`, `map_value`; codec `parse`, `parse_prefix`,
`encode`, `encode_resp2`, `free_value`.
