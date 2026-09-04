# std.brotli

Brotli compression (RFC 7932) — streaming and one-shot.

`br` is what current browsers actually prefer: every modern browser lists it
ahead of `gzip` in `Accept-Encoding`, and it typically beats gzip by 15–25% on
text at comparable speed. This module wraps the system **libbrotlienc**
(`apt install libbrotli-dev`, `brew install brotli`).

Compression only. An HTTP server compresses responses; nothing in-tree needs to
*decode* `br`, and a decoder is separate work.

## Streaming

The surface deliberately mirrors `std.zlib`'s streaming deflate, so a server
negotiating `br` against `gzip` writes the same shape either way:

```
s, err        = brotli.stream_new(brotli.FAST_QUALITY, 0)
chunk, n, err = brotli.stream_write(s, ev, string.length(ev))
chunk, n, err = brotli.stream_flush(s)      // send these bytes now
tail,  n, err = brotli.stream_finish(s)     // at connection close
                brotli.stream_free(s)
```

`stream_write` usually returns **0 bytes** — the encoder buffers internally for
compression ratio. `stream_flush` is what emits a decodable boundary while
keeping the window, so later events cost far less than the first: three
repetitive events compress to 31, 13 and 13 bytes.

A handle owns its output buffer, so two streams may be open on one thread (two
SSE connections on one event loop) without fighting over it. Always
`stream_free`.

## Quality

`quality` is 0–11. **11 is the library default and is slow** — `FAST_QUALITY`
(5) is the usual choice for a live HTTP response. `window` is the lg2 window
size (10–24); pass 0 for the library default. Both are clamped rather than
rejected, matching how `std.zlib` treats `level`.

## One-shot

```
packed, n, err = brotli.compress(body, string.length(body), brotli.FAST_QUALITY)
```

## When the backend is absent

Built without libbrotlienc, `available()` returns 0 and `stream_new` returns
`(null, "brotli unavailable")`. Every other stream call needs the handle, so a
program fails where it *asks for* a stream rather than silently emitting
nothing. A server negotiating content-encoding should check `available()` and
fall back to gzip.

## Exports

`available`, `compress`, `stream_new`, `stream_write`, `stream_flush`,
`stream_finish`, `stream_free` (and the `MIN_QUALITY` / `MAX_QUALITY` /
`DEFAULT_QUALITY` / `FAST_QUALITY` constants), plus the `brotli_*` raw forms.
