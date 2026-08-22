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
    src = "abcabcabcabcabcabcabcabcabcabcabcabc"
    n = string.length(src)

    packed, plen, cerr = zlib.deflate(src, n, 6)
    println("compressed ${n} -> ${plen} err='${cerr}'")

    // inflate does not need the original length — the zlib
    // container carries what it needs.
    back, blen, derr = zlib.inflate(packed, plen)
    println("round trip ok: ${string.equals(back, src)} err='${derr}'")
}
```
```output
compressed 36 -> 13 err=''
round trip ok: 1 err=''
```

`available()` reports whether the backend was built in. It is an optional
dependency, so a program that must degrade gracefully should check rather than
assume — every entry point returns `"zlib unavailable"` when it is absent.

Unlike `std.lzf`, deflating an incompressible input succeeds and simply
produces slightly more bytes than it consumed.

## Exports

`available`, `deflate`, `inflate`, `gzip_deflate`, `gzip_inflate`, plus the
`zlib_*` raw forms.
