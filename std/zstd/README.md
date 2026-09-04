# std.zstd

Zstandard compression (RFC 8878) — streaming and one-shot.

Zstandard is a different **format** from DEFLATE, not a faster implementation
of it: this wraps **libzstd** (`apt install libzstd-dev`, `brew install zstd`)
and has nothing to do with `std.zlib` beyond the similar library name.

Its case is strongest away from the browser — archives, logs, snapshots,
internal RPC — because `Content-Encoding: zstd` support is still thinner than
`br` and `gzip`. It compresses harder than gzip at comparable speed, and
decompresses faster than both.

Compression only. Nothing in-tree needs to *decode* zstd, and a decoder is
separate work.

## Streaming

The surface mirrors `std.zlib`'s and `std.brotli`'s, so a caller choosing an
encoding writes the same shape whichever it picks:

```
s, err        = zstd.stream_new(zstd.DEFAULT_LEVEL)
chunk, n, err = zstd.stream_write(s, ev, string.length(ev))
chunk, n, err = zstd.stream_flush(s)      // send these bytes now
tail,  n, err = zstd.stream_finish(s)     // close the frame
                zstd.stream_free(s)
```

`stream_write` usually returns **0 bytes** — the encoder buffers internally for
compression ratio. `stream_flush` emits a decodable boundary while keeping the
window, so later events cost far less than the first: three repetitive events
compress to 37, 12 and 13 bytes.

A handle owns its output buffer, so two streams may be open on one thread
without fighting over it. Always `stream_free`.

## Level

`level` is 1–22; **3 is libzstd's default** and a good general choice. Higher
levels cost markedly more time for modest gains. Out-of-range values are
clamped rather than rejected, matching `std.zlib`'s treatment of `level`.

## One-shot

```
packed, n, err = zstd.compress(body, string.length(body), zstd.DEFAULT_LEVEL)
```

## When the backend is absent

Built without libzstd, `available()` returns 0 and `stream_new` returns
`(null, "zstd unavailable")`. Every other stream call needs the handle, so a
program fails where it *asks for* a stream rather than silently emitting
nothing.

## Exports

`available`, `compress`, `stream_new`, `stream_write`, `stream_flush`,
`stream_finish`, `stream_free` (and the `MIN_LEVEL` / `MAX_LEVEL` /
`DEFAULT_LEVEL` constants), plus the `zstd_*` raw forms.
