# std.zlib

DEFLATE compression, in both the zlib and gzip framings.

`deflate`/`inflate` use the zlib container; `gzip_deflate`/`gzip_inflate` use
the gzip one. **They are not interchangeable** — feeding a raw deflate stream
to `gzip_inflate` is an error rather than a silent misparse, which is what you
want when the framing is decided by a `Content-Encoding` header.

Compression level runs 0 (store) to 9 (maximum); -1 selects the library
default.

```aether,run
import std.zlib
import std.string

main() {
    // The backend is an optional build dependency, so a program that
    // must degrade gracefully checks before compressing rather than
    // reading "zlib unavailable" out of every call.
    if zlib.available() == 0 {
        println("round trip ok: skipped (no zlib backend)")
        return
    }

    src = "abcabcabcabcabcabcabcabcabcabcabcabc"
    n = string.length(src)

    packed, plen, cerr = zlib.deflate(src, n, 6)
    if cerr != "" {
        println("deflate failed: ${cerr}")
        return
    }

    // inflate does not need the original length — the zlib
    // container carries what it needs.
    back, blen, derr = zlib.inflate(packed, plen)
    println("round trip ok: ${string.equals(back, src)} err='${derr}'")
}
```
```output
round trip ok: 1 err=''
```

That guard is not decoration: it is what makes the example's output the same
whether or not the machine running it has the backend, and it is the shape a
caller should copy.

Every entry point returns `"zlib unavailable"` when the backend is absent, so
an unchecked caller gets an error rather than a wrong result — but checking
`available()` once is clearer than threading that error through every call.

Unlike `std.lzf`, deflating an incompressible input succeeds and simply
produces slightly more bytes than it consumed.

## Streaming deflate

The calls above are one-shot: each emits a **complete** stream. That is wrong
for a long-lived response. An SSE connection carrying many small events needs
ONE deflate stream held open for the life of the connection, flushed at each
event boundary, so the client sees a single continuous stream. Compressing each
event independently produces N complete streams concatenated, which no
`Content-Encoding: gzip` client will decode.

```
s, err = zlib.stream_new(zlib.GZIP, 6)      // RAW / ZLIB / GZIP
chunk, n, err = zlib.stream_write(s, ev, string.length(ev))
chunk, n, err = zlib.stream_flush(s)        // send these bytes now
// ... more events, same stream ...
tail, n, err = zlib.stream_finish(s)        // at connection close
zlib.stream_free(s)
```

`stream_write` usually returns **0 bytes** — deflate buffers internally for
compression ratio. `stream_flush` is what emits a decodable boundary while
keeping the window, so later events cost far less than the first: three
repetitive events in the test compress to 34, 12 and 11 bytes. Emit whatever
each call returns, and always `stream_free` the handle.

A handle owns its own output buffer, so two streams may be open on one thread
(two SSE connections on one event loop) without fighting over it.

Streaming *inflate* is not implemented; the one-shot `inflate` / `gzip_inflate`
read a complete stream, including one assembled from flushed chunks.

## Exports

`available`, `deflate`, `inflate`, `gzip_deflate`, `gzip_inflate`,
`stream_new`, `stream_write`, `stream_flush`, `stream_finish`, `stream_free`
(and the `RAW` / `ZLIB` / `GZIP` format constants), plus the `zlib_*` raw
forms.
