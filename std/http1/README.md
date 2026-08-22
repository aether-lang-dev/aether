# std.http1

The HTTP/1.1 wire format, on its own.

Parsing a status line, parsing headers, feeding bytes incrementally until a
response is complete. `std.http`'s client and server use this; it is exposed
separately for code that has bytes from somewhere else — a proxy relaying a
response, a test double, a protocol that tunnels HTTP inside itself.

The example **compiles but is not run** in CI: it needs a live connection.

```aether
import std.http1
import std.bytes
import std.string

main() {
    resp = http1.response_new()

    // feed takes a chunk and reports "complete", "incomplete", or an
    // error — hand it whatever bytes have arrived so far.
    head = "HTTP/1.1 200 OK\r\n\r\n"
    chunk = bytes.new(64)
    bytes.copy_from_string(chunk, 0, head, string.length(head))

    state = http1.feed(resp, chunk, string.length(head), 0)
    println("state: ${state}")

    http1.response_free(resp)
}
```

`feed` is the incremental entry point, and the reason to use this module: it
returns `"complete"`, `"incomplete"` or an error, so a caller parses as bytes
arrive rather than buffering a whole response first. `is_eof` tells it whether
more can still come, which is what distinguishes a truncated response from one
that is merely unfinished.

`parse_status_line` and `parse_headers` are the lower-level pieces, operating
on a response struct and a raw buffer — reach for them only when `feed`'s
framing is not what you want.

`read_response_conn` is the convenience form when you do have a connection and
want the whole response.

For ordinary HTTP work use `std.http`. This module is for when you are
implementing something HTTP-shaped rather than speaking HTTP.

## Exports

`HttpResponse`, `response_new`, `response_free`, `parse_status_line`,
`parse_headers`, `feed`, `read_response_conn`.
