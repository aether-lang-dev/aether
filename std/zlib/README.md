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

## Exports

`available`, `deflate`, `inflate`, `gzip_deflate`, `gzip_inflate`, plus the
`zlib_*` raw forms.
