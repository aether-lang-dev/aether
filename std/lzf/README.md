# std.lzf

LZF compression: very fast, modest ratio.

LZF trades compression ratio for speed — it is the algorithm to reach for when
the cost of compressing must stay near the cost of copying, such as an
in-memory cache or a value on its way to a socket you already know is the
bottleneck. For archival or wire formats where size matters more, `std.zlib`
compresses harder.

**LZF refuses any input it cannot shrink.** Not a length floor — a
compressibility one: `compress` returns an error rather than emitting a block
larger than its input, so already-compressed or random data is declined. The
error is opaque (`"lzf compress failed"`), so a caller feeding it arbitrary
payloads should treat it as "store this uncompressed" rather than a fault.

```aether,run
import std.lzf
import std.string

main() {
    src = "abcabcabcabcabcabcabcabcabcabcabcabc"
    n = string.length(src)

    packed, plen, cerr = lzf.compress(src, n)
    println("compressed ${n} -> ${plen} err='${cerr}'")

    // decompress needs the ORIGINAL length: the format does not
    // carry it, so the caller has to have kept it.
    back, blen, derr = lzf.decompress(packed, plen, n)
    println("round trip ok: ${string.equals(back, src)} err='${derr}'")
}
```
```output
compressed 36 -> 11 err=''
round trip ok: 1 err=''
```

`max_compressed_size(n)` gives the worst-case output bound, which exceeds `n` —
useful for sizing a buffer, though this API allocates for you.

## Exports

`compress`, `decompress`, `max_compressed_size`, plus the `lzf_*` raw forms.
